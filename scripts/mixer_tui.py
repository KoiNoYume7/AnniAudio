#!/usr/bin/env python3
"""
AnniAudio mixer TUI (applications -> virtual cables -> outputs).

A curses front-end for the mixer control API embedded in `route_cli mixer`
(see docs/MIXER-CONTROL-API.md for the full HTTP/SSE surface). It never touches
audio itself; every action is a REST call, and every state update flows back in
over Server-Sent Events, so this window always mirrors whatever the mixer
process (and any other connected client) is actually doing.

Threading model:
    - The main thread owns curses and only ever reads `state`/`endpoints`/etc.
      under `state_lock` before drawing a frame; it never blocks on the network.
    - `sse_worker` holds a long-lived GET to /api/events and replaces `state`
      wholesale each time a `data:` event arrives, reconnecting on drop.
    - `api_worker` drains `api_queue` and fires the PATCH/POST/DELETE calls
      queued by `api_call()`, so a slow request (e.g. one that opens/closes a
      WASAPI device) never freezes the UI.
    - `job_worker` drains `job_queue` and runs the slow multi-step actions
      (adding an app, changing a group cable, refreshing endpoint lists) that
      mix HTTP and COM calls; the header shows "working..." while one runs.

Keybindings:
    Tab          switch between the Virtual Cables and Outputs panes
    j/k, Up/Down move the selection
    PgUp/PgDn    page the selection
    +/-          nudge the selected virtual cable / output volume by 5%
    v            type an exact volume (0-200%)
    m            toggle mute on the selected virtual cable
    Enter/e      expand/collapse a virtual cable's application list
    i            add a device source (from a device picker) to the selected virtual cable
    a            add an application (from a running app picker) to the selected virtual cable
    g            add a new virtual cable
    C            set the selected virtual cable's group cable (render VAC) for per-app routing
    o            add a new output (from a render-endpoint picker)
    c            connect/disconnect the selected virtual cable and output
    r            rename the selected virtual cable
    d            delete the selected virtual cable/application/output
    s            save the current state as a preset (empty path = autosave)
    R            refresh the endpoint and application lists from the mixer
    q            quit (Esc only cancels dialogs)

Usage:
    python scripts/mixer_tui.py [--port 8850]

Requires the route_cli mixer to already be running on the same port (see
start-mixer.bat / mixer-tui.bat for the usual two-step launch).
"""

import argparse
import json
import math
import os
import queue
import sys
import threading
import time
import traceback

import requests

try:
    import curses
except ImportError:
    print("The 'curses' module is not available. On Windows install it with:")
    print("    python -m pip install windows-curses")
    sys.exit(1)

try:
    import winappaudiorouter
    _HAS_ROUTER = True
except Exception:
    winappaudiorouter = None
    _HAS_ROUTER = False

BASE = "http://127.0.0.1:{port}"

# Shared state, protected by state_lock. Written by the background threads
# below and read once per frame by the render loop in main().
state_lock = threading.Lock()
state = {"running": False, "inputs": [], "groups": [], "outputs": []}
endpoints = []
applications = []
# input_id -> (pid, cable, group_id) for app inputs that were routed to a VAC.
# Used to clear the per-app route when the input is removed, re-route inputs
# when a group's cable changes, and to repair routes that Windows overrides
# (e.g. in the volume mixer).
routed_apps = {}
_route_running = threading.Event()
status_text = "Connecting..."
last_error = ""
last_success = ""

# Fire-and-forget PATCH/POST/DELETE requests, drained by api_worker().
api_queue = queue.Queue()

# Slow multi-step actions (add app, set cable, refresh lists...) run as jobs on
# job_worker() so the render loop never blocks on HTTP or COM calls. busy_jobs
# (guarded by state_lock) drives the "working..." indicator in the header.
job_queue = queue.Queue()
busy_jobs = 0

SELECTED_PAIR = 1
METER_PAIR = 2
MUTED_PAIR = 3
HEADER_PAIR = 4
SUCCESS_PAIR = 5
ERROR_PAIR = 6
GROUP_PAIR_START = 10

# ---------------------------------------------------------------------------
# API worker
# ---------------------------------------------------------------------------

def api_worker(port):
    """Background thread: serialize queued API calls so the render loop never blocks on I/O."""
    global last_error, last_success
    base = BASE.format(port=port)
    session = requests.Session()
    while True:
        task = api_queue.get()
        if task is None:
            break
        method, path, body = task
        try:
            # Long timeout for structural changes that open/close WASAPI devices.
            kwargs = {"timeout": (5, 60)}
            if body is not None:
                kwargs["json"] = body
            r = session.request(method, base + path, **kwargs)
            if not r.ok:
                try:
                    msg = r.json().get("error", r.text)
                except Exception:
                    msg = r.text or r.reason
                with state_lock:
                    last_error = f"{method} {path} -> {r.status_code}: {msg}"
            else:
                with state_lock:
                    last_success = f"{method} {path} OK"
        except Exception as e:
            with state_lock:
                last_error = f"{method} {path} -> {e}"


def api_call(method, path, body=None):
    """Queue a mutating API request; the result (if any) arrives later via SSE."""
    api_queue.put((method, path, body))


def job_worker():
    """Background thread: run slow multi-step actions queued by run_job()."""
    global last_error, busy_jobs
    while True:
        job = job_queue.get()
        if job is None:
            break
        try:
            job()
        except Exception as e:
            with state_lock:
                last_error = f"Action failed: {e}"
        finally:
            with state_lock:
                busy_jobs -= 1


def run_job(fn):
    """Queue a slow action for job_worker so the UI keeps rendering meanwhile."""
    global busy_jobs
    with state_lock:
        busy_jobs += 1
    job_queue.put(fn)


def _route_worker(port):
    """Background thread: repair per-app routes that Windows overrides.

    Every few seconds it checks every tracked app route. If the current route
    does not match the cable we expect, it re-applies it. This keeps Spotify/etc.
    on the chosen VAC even if the volume mixer is touched.
    """
    global last_error, last_success
    while _route_running.is_set():
        # Work on a snapshot so we do not hold state_lock during COM calls.
        with state_lock:
            routes = list(routed_apps.items())
        for iid, (pid, cable, gid) in routes:
            if not cable or not _HAS_ROUTER or not winappaudiorouter:
                continue
            try:
                with winappaudiorouter.com.com_initialized():
                    current = winappaudiorouter.get_app_output_device(process_id=pid)
                    target = winappaudiorouter.find_output_device(cable)
                    current_id = current.get(pid) if current else None
                    if current_id != target.id:
                        winappaudiorouter.set_app_output_device(process_id=pid, device=cable)
                        with state_lock:
                            last_success = f"Repaired route for pid {pid} -> {cable}"
            except Exception as e:
                with state_lock:
                    last_error = f"Route repair failed for pid {pid}: {e}"
        # Wait in small chunks so we can exit quickly.
        for _ in range(10):
            if not _route_running.is_set():
                break
            time.sleep(0.5)


# ---------------------------------------------------------------------------
# SSE / polling worker
# ---------------------------------------------------------------------------

def sse_worker(port):
    """Background thread: keep `state` in sync via /api/events, reconnecting on drop."""
    global status_text, state
    base = BASE.format(port=port)
    session = requests.Session()
    while True:
        try:
            with state_lock:
                status_text = "SSE connecting..."
            r = session.get(base + "/api/events", stream=True, timeout=(5, None))
            if r.status_code != 200:
                with state_lock:
                    status_text = f"SSE {r.status_code}"
                time.sleep(2)
                continue
            with state_lock:
                status_text = "Connected"
            for line in r.iter_lines():
                if line is None:
                    continue
                if line.startswith(b":"):
                    continue
                if line.startswith(b"data: "):
                    try:
                        payload = json.loads(line[6:])
                        with state_lock:
                            state = payload
                    except Exception as e:
                        with state_lock:
                            status_text = f"SSE parse error: {e}"
        except Exception as e:
            with state_lock:
                status_text = f"SSE error: {e}"
            time.sleep(2)


def fetch_endpoints(port):
    """Pull the current WASAPI endpoint list (used by the 'i'/'o' device pickers).

    Not pushed via SSE, so this is called once at startup and again on demand
    when the user presses 'R' (e.g. after plugging in a new device).
    """
    global endpoints
    try:
        r = requests.get(BASE.format(port=port) + "/api/endpoints", timeout=15)
        if r.ok:
            with state_lock:
                endpoints = r.json().get("endpoints", [])
    except Exception:
        with state_lock:
            endpoints = []


def fetch_applications(port):
    """Pull the current running audio application sessions (used by the 'a' picker)."""
    global applications
    try:
        r = requests.get(BASE.format(port=port) + "/api/applications", timeout=15)
        if r.ok:
            with state_lock:
                applications = r.json().get("applications", [])
    except Exception:
        with state_lock:
            applications = []


def apps_refresh_worker(port):
    """Background thread: keep the application list fresh (every 5 seconds).

    The route-mismatch warnings compare against this list; refreshing it
    automatically means they clear on their own after an app restarts, instead
    of lying until the user presses R.
    """
    while _route_running.is_set():
        fetch_applications(port)
        for _ in range(10):
            if not _route_running.is_set():
                return
            time.sleep(0.5)


def _add_input_to_group(port, group, name, input_type, source):
    """Create an input and attach it to the selected virtual cable. Returns the new input id."""
    global last_error
    try:
        r = requests.post(BASE.format(port=port) + "/api/inputs",
                          json={"name": name, "type": input_type, "source": source}, timeout=15)
        if r.ok:
            inp = r.json()
            iid = inp.get("id")
            if iid is not None:
                ids = list(group.get("inputIds", []))
                if iid not in ids:
                    ids.append(iid)
                    api_call("PATCH", f"/api/groups/{group['id']}", {"inputIds": ids})
            return iid
        else:
            with state_lock:
                try:
                    last_error = r.json().get("error", str(r.status_code))
                except Exception:
                    last_error = str(r.status_code)
    except Exception as e:
        with state_lock:
            last_error = str(e)
    return None


def _route_app_to_cable(pid, cable_name):
    """Use winappaudiorouter to set an app's default output to a VAC."""
    if not _HAS_ROUTER or not winappaudiorouter:
        return False, "winappaudiorouter not installed"
    try:
        with winappaudiorouter.com.com_initialized():
            winappaudiorouter.set_app_output_device(process_id=pid, device=cable_name)
        return True, None
    except Exception as e:
        return False, str(e)


def _clear_app_route(pid):
    """Clear a per-app route set by winappaudiorouter."""
    if not _HAS_ROUTER or not winappaudiorouter:
        return False, "winappaudiorouter not installed"
    try:
        with winappaudiorouter.com.com_initialized():
            winappaudiorouter.clear_app_output_device(process_id=pid)
        return True, None
    except Exception as e:
        return False, str(e)


def _reroute_input_to_cable(port, input_id, pid, new_cable, input_name):
    """Point an existing app input at a new VAC capture source.

    Returns True on success. On failure, last_error is set under state_lock.
    """
    global last_error
    ok, err = _route_app_to_cable(pid, new_cable)
    if not ok:
        with state_lock:
            last_error = f"Could not route {input_name} to {new_cable}: {err}"
        return False
    try:
        r = requests.patch(
            BASE.format(port=port) + f"/api/inputs/{input_id}",
            json={"name": input_name, "type": "device", "source": new_cable},
            timeout=15,
        )
        if not r.ok:
            with state_lock:
                try:
                    last_error = r.json().get("error", str(r.status_code))
                except Exception:
                    last_error = str(r.status_code)
            return False
        return True
    except Exception as e:
        with state_lock:
            last_error = str(e)
    return False


def _reroute_group_apps(port, group, new_cable, state):
    """Re-apply per-app routing for every app-like input in a group.

    Called when a group's cable changes. Loopback inputs become VAC inputs and
    VAC inputs move to the new cable. Inputs not tracked in routed_apps are also
    handled if they look like application inputs (type == "application") so that
    configs from previous sessions or other clients still get repaired.
    """
    global last_error, last_success
    group_id = group.get("id")
    if not group_id:
        return

    inputs_by_id = {inp["id"]: inp for inp in state.get("inputs", [])}
    re_routed = 0
    failed = 0
    processed = set()

    def re_route_one(iid, pid, old_cable, inp_name, inp_type, inp_source):
        global last_error
        nonlocal re_routed, failed
        processed.add(iid)
        if not new_cable:
            # Cable cleared: move back to loopback capture and clear the route.
            _clear_app_route(pid)
            try:
                r = requests.patch(
                    BASE.format(port=port) + f"/api/inputs/{iid}",
                    json={"name": inp_name or str(pid), "type": "application", "source": str(pid)},
                    timeout=15,
                )
                if r.ok:
                    routed_apps[iid] = (pid, "", group_id)
                    re_routed += 1
                else:
                    failed += 1
            except Exception as e:
                with state_lock:
                    last_error = str(e)
                failed += 1
            return

        if old_cable == new_cable and inp_type == "device" and inp_source == new_cable:
            # Already correct.
            return

        if _reroute_input_to_cable(port, iid, pid, new_cable, inp_name or str(pid)):
            routed_apps[iid] = (pid, new_cable, group_id)
            re_routed += 1
        else:
            failed += 1

    # First pass: inputs currently in the group.
    for iid in group.get("inputIds", []):
        inp = inputs_by_id.get(iid, {})
        name = inp.get("name", "")
        itype = inp.get("type", "")
        source = inp.get("source", "")

        pid = None
        old_cable = ""
        if iid in routed_apps:
            pid, old_cable, _gid = routed_apps[iid]
        elif itype == "application":
            try:
                pid = int(source)
                old_cable = ""
            except ValueError:
                pid = None

        if pid is None:
            continue
        re_route_one(iid, pid, old_cable, name, itype, source)

    # Second pass: any tracked apps for this group that may still be pending in
    # api_worker (so they are not in group.get("inputIds") yet).
    for iid, (pid, old_cable, gid) in list(routed_apps.items()):
        if gid != group_id or iid in processed:
            continue
        inp = inputs_by_id.get(iid, {})
        re_route_one(iid, pid, old_cable, inp.get("name", ""), inp.get("type", ""), inp.get("source", ""))

    with state_lock:
        if re_routed and not failed:
            last_success = f"Re-routed {re_routed} app(s) to {new_cable}"
        elif failed:
            last_error = f"Re-routed {re_routed} app(s), {failed} failed"


def _set_group_cable(port, group, cable):
    """Set a group's cable and re-route its app inputs. Runs on job_worker."""
    global last_error, last_success
    try:
        r = requests.patch(
            BASE.format(port=port) + f"/api/groups/{group['id']}",
            json={"cable": cable},
            timeout=15,
        )
        if not r.ok:
            with state_lock:
                try:
                    last_error = r.json().get("error", str(r.status_code))
                except Exception:
                    last_error = str(r.status_code)
            return
    except Exception as e:
        with state_lock:
            last_error = str(e)
        return

    # Re-route any app inputs that were already in this group.
    with state_lock:
        current_state = state
    _reroute_group_apps(port, group, cable, current_state)
    with state_lock:
        if not last_error:
            if cable:
                last_success = f"Group cable set to {cable}; existing apps re-routed"
            else:
                last_success = "Group cable cleared; existing apps use loopback"


def _route_sidecar_path(port):
    """Path to a small JSON sidecar that persists routed_apps across TUI runs."""
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), f".routed_apps_{port}.json")


def _load_tracked_routes(port, state):
    """Restore routed_apps from disk, reconciling against the current state."""
    path = _route_sidecar_path(port)
    if not os.path.exists(path):
        return
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception:
        return

    inputs_by_id = {inp["id"]: inp for inp in state.get("inputs", [])}
    groups = {g["id"]: g for g in state.get("groups", [])}

    for entry in data.get("routes", []):
        iid = entry.get("input_id")
        gid = entry.get("group_id")
        name = entry.get("name", "")
        pid = entry.get("pid")
        cable = entry.get("cable", "")

        # Exact input id still present in the same group.
        if iid in inputs_by_id:
            g = groups.get(gid)
            if g and iid in g.get("inputIds", []):
                routed_apps[iid] = (pid, cable, gid)
                continue

        # Reconcile by group + name if the id shifted (config was edited).
        g = groups.get(gid)
        if not g:
            continue
        for iid2 in g.get("inputIds", []):
            inp2 = inputs_by_id.get(iid2, {})
            if inp2.get("name") == name:
                routed_apps[iid2] = (pid, cable, gid)
                break


def _save_tracked_routes(port, state):
    """Persist routed_apps so the next TUI run can repair routes."""
    inputs_by_id = {inp["id"]: inp for inp in state.get("inputs", [])}

    routes = []
    for iid, (pid, cable, gid) in routed_apps.items():
        inp = inputs_by_id.get(iid, {})
        name = inp.get("name", "")
        # Confirm the input is still in the group we recorded; otherwise the
        # group_id stored in routed_apps is the authoritative source of truth
        # (e.g. for inputs not yet added to the group by api_worker).
        routes.append({"input_id": iid, "group_id": gid, "pid": pid, "cable": cable, "name": name})

    path = _route_sidecar_path(port)
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"port": port, "routes": routes}, f, indent=2)
    except Exception:
        pass


def _assign_app_to_group(port, group, app):
    """Assign a running application to a group.

    If the group has a cable configured, the app's per-app output is routed to
    that cable and the cable is added as a device input. If not, the app is
    captured via process loopback (with double-audio side effect).
    """
    global last_error, last_success
    pid = app.get("processId")
    name = app.get("name", str(pid))
    cable = group.get("cable", "")
    group_id = group.get("id")

    # Reuse an existing matching input instead of stacking duplicates: repeated
    # 'a' presses used to add a new input every time (five Spotify entries...).
    def existing_group_input(match_type, match_source):
        with state_lock:
            snap = state
        inputs_by_id = {i.get("id"): i for i in snap.get("inputs", [])}
        for iid in group.get("inputIds", []):
            inp = inputs_by_id.get(iid, {})
            if inp.get("type") == match_type and inp.get("source") == match_source:
                return iid
        return None

    if cable:
        ok, err = _route_app_to_cable(pid, cable)
        if not ok:
            with state_lock:
                last_error = f"Could not route {name} to {cable}: {err}"
            return
        # Capture from the same-named VAC capture endpoint by using the cable
        # name as the source. AudioMixer::findAnyDevice prefers capture when
        # both render and capture share a friendly name.
        iid = existing_group_input("device", cable)
        if iid is None:
            iid = _add_input_to_group(port, group, name, "device", cable)
        if iid is not None:
            if iid not in routed_apps:
                routed_apps[iid] = (pid, cable, group_id)
            with state_lock:
                last_success = f"Routed {name} -> {cable} (restart the app if it does not switch)"
        else:
            with state_lock:
                last_error = f"Routed {name} but could not add {cable} input"
    else:
        iid = existing_group_input("application", str(pid))
        if iid is not None:
            with state_lock:
                last_success = f"{name} is already captured by this group"
            return
        iid = _add_input_to_group(port, group, name, "application", str(pid))
        if iid is not None:
            routed_apps[iid] = (pid, "", group_id)
        with state_lock:
            if iid is not None:
                last_success = f"Added {name} via loopback (no cable: double audio)"
            else:
                last_error = f"Could not add {name}"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def level_to_pct(v):
    """Map a linear 0-1 peak/RMS sample to a 0-100 meter-bar percentage.

    Uses a 60 dB display range (-60 dBFS -> 0%, 0 dBFS -> 100%); this is a
    display curve for the meter bars, not a calibrated loudness measurement.
    """
    if v <= 0.00001:
        return 0
    db = 20 * math.log10(min(v, 1.0))
    return max(0, min(100, (db + 60) / 60 * 100))


def meter_bar(pct, width=10):
    """Render a 0-100 percentage as a filled/empty block-character bar."""
    filled = int(round(pct / 100 * width))
    return "█" * filled + "░" * (width - filled)


def vol_bar(vol, width=16):
    """Render a 0-200 volume percentage as a filled/empty block-character bar."""
    filled = int(round(vol / 200 * width))
    return "█" * filled + "░" * (width - filled)


def clamp_vol(v):
    """Clamp a volume percentage to the 0-200% range the mixer API accepts."""
    return max(0, min(200, v))


def hex_to_curses_color(hexstr):
    """Map a '#rrggbb' group color to the nearest of curses' 8 base colors.

    Most terminals only reliably support 8 (or 16) colors, so this picks the
    closest match by squared RGB distance rather than assuming 256-color support.
    """
    hexstr = hexstr.lstrip("#")
    if len(hexstr) != 6:
        return curses.COLOR_WHITE
    try:
        r = int(hexstr[0:2], 16)
        g = int(hexstr[2:4], 16)
        b = int(hexstr[4:6], 16)
    except Exception:
        return curses.COLOR_WHITE
    candidates = [
        (curses.COLOR_BLACK, 0, 0, 0),
        (curses.COLOR_RED, 255, 0, 0),
        (curses.COLOR_GREEN, 0, 255, 0),
        (curses.COLOR_YELLOW, 255, 255, 0),
        (curses.COLOR_BLUE, 0, 0, 255),
        (curses.COLOR_MAGENTA, 255, 0, 255),
        (curses.COLOR_CYAN, 0, 255, 255),
        (curses.COLOR_WHITE, 255, 255, 255),
    ]
    best = curses.COLOR_WHITE
    best_d = 1e9
    for cid, cr, cg, cb in candidates:
        d = (r - cr) ** 2 + (g - cg) ** 2 + (b - cb) ** 2
        if d < best_d:
            best_d = d
            best = cid
    return best


def group_color_attr(color, pair_cache, selected=False):
    """Return a curses attr for a group's color, lazily allocating a color pair.

    Curses has a limited number of color pairs, so pair_cache (keyed by hex
    color, shared across a single run() call) allocates one pair per distinct
    color the first time it's seen and reuses it afterward.
    """
    if not curses.has_colors():
        return curses.A_REVERSE if selected else curses.A_NORMAL
    pair_id = pair_cache.get(color)
    if pair_id is None:
        pair_id = GROUP_PAIR_START + len(pair_cache)
        fg = hex_to_curses_color(color)
        try:
            curses.init_pair(pair_id, fg, -1)
        except Exception:
            pass
        pair_cache[color] = pair_id
    attr = curses.color_pair(pair_id)
    if selected:
        attr |= curses.A_REVERSE
    return attr


def input_for_id(iid, local_state):
    """Look up an input dict by id within a state snapshot; O(n), state is small."""
    for inp in local_state.get("inputs", []):
        if inp.get("id") == iid:
            return inp
    return None


def group_for_id(gid, local_state):
    """Look up a group dict by id within a state snapshot; O(n), state is small."""
    for g in local_state.get("groups", []):
        if g.get("id") == gid:
            return g
    return None


# ---------------------------------------------------------------------------
# Dialogs
#
# Each of these owns a modal curses window and blocks (via win.getch(), which
# defaults to blocking mode unlike stdscr's 50ms timeout) until the user
# confirms or cancels. The main render loop is paused for the duration.
# ---------------------------------------------------------------------------

def draw_box(win, title=""):
    """Draw a bordered box with an optional centered title on a curses window."""
    h, w = win.getmaxyx()
    try:
        win.box()
    except curses.error:
        pass
    if title:
        try:
            win.addstr(0, 2, f" {title} ", curses.A_BOLD)
        except curses.error:
            pass


def list_dialog(stdscr, title, items, start_idx=0):
    """Modal scrollable picker with type-to-filter.

    Typing narrows the list (case-insensitive substring match); Backspace edits
    the filter, Up/Down navigate, Enter confirms, Esc cancels. Returns the
    selected index into the ORIGINAL items list, or None if cancelled.
    """
    if not items:
        return None
    h, w = stdscr.getmaxyx()
    max_h = min(h - 4, max(10, len(items) + 5))
    max_w = min(w - 4, max(44, max(len(str(x)) for x in items) + 8))
    y = (h - max_h) // 2
    x = (w - max_w) // 2
    win = curses.newwin(max_h, max_w, y, x)
    win.keypad(True)
    idx = start_idx
    offset = 0
    filt = ""
    while True:
        if filt:
            matches = [i for i, it in enumerate(items) if filt.lower() in str(it).lower()]
        else:
            matches = list(range(len(items)))
        if matches:
            idx = min(idx, len(matches) - 1)
        win.erase()
        draw_box(win, title)
        prompt = f"filter: {filt}" if filt else "type to filter, ↑↓ navigate"
        try:
            win.addstr(1, 2, prompt[:max_w - 4], curses.A_DIM if not filt else curses.A_BOLD)
        except curses.error:
            pass
        visible = max_h - 5
        # Keep the selection inside the visible window without recentering it
        # on every keypress (a moving offset makes the list feel jumpy).
        if idx < offset:
            offset = idx
        elif idx >= offset + visible:
            offset = idx - visible + 1
        for i in range(visible):
            line = offset + i
            if line >= len(matches):
                break
            attr = curses.A_NORMAL
            if line == idx:
                attr = curses.color_pair(SELECTED_PAIR) | curses.A_BOLD
            try:
                win.addstr(3 + i, 2, str(items[matches[line]])[:max_w - 4], attr)
            except curses.error:
                pass
        if not matches:
            try:
                win.addstr(3, 2, "(no matches)", curses.A_DIM)
            except curses.error:
                pass
        try:
            win.refresh()
        except curses.error:
            pass
        ch = win.getch()
        if ch == curses.KEY_UP:
            if matches:
                idx = (idx - 1) % len(matches)
        elif ch == curses.KEY_DOWN:
            if matches:
                idx = (idx + 1) % len(matches)
        elif ch in (curses.KEY_ENTER, 10, 13):
            if matches:
                return matches[idx]
        elif ch == 27:
            return None
        elif ch == curses.KEY_BACKSPACE or ch == 127 or ch == 8:
            filt = filt[:-1]
            idx = 0
        elif 32 <= ch <= 126:
            filt += chr(ch)
            idx = 0


def input_dialog(stdscr, title, default=""):
    """Modal single-line text prompt. Returns the entered text, or None if cancelled.

    Only accepts printable ASCII (32-126); there is no Unicode input support.
    """
    h, w = stdscr.getmaxyx()
    height = 5
    width = min(w - 4, max(50, len(title) + 10))
    y = (h - height) // 2
    x = (w - width) // 2
    win = curses.newwin(height, width, y, x)
    win.keypad(True)
    text = default
    try:
        curses.curs_set(1)
    except curses.error:
        pass
    try:
        while True:
            win.erase()
            draw_box(win, title)
            shown = text[:width - 4]
            try:
                win.addstr(2, 2, shown)
                win.move(2, 2 + len(shown))
                win.refresh()
            except curses.error:
                pass
            ch = win.getch()
            if ch in (curses.KEY_ENTER, 10, 13):
                return text
            elif ch in (27,):
                return None
            elif ch == curses.KEY_BACKSPACE or ch == 127 or ch == 8:
                text = text[:-1]
            elif 32 <= ch <= 126:
                text += chr(ch)
    finally:
        try:
            curses.curs_set(0)
        except curses.error:
            pass


def confirm_dialog(stdscr, text):
    """Modal y/n prompt. Returns True/False; Esc counts as No."""
    h, w = stdscr.getmaxyx()
    height = 5
    width = min(w - 4, max(40, len(text) + 10))
    y = (h - height) // 2
    x = (w - width) // 2
    win = curses.newwin(height, width, y, x)
    win.keypad(True)
    while True:
        win.erase()
        draw_box(win, "Confirm")
        try:
            win.addstr(2, 2, text[:width - 4])
            win.refresh()
        except curses.error:
            pass
        ch = win.getch()
        if ch in (ord('y'), ord('Y'), 10, 13):
            return True
        elif ch in (ord('n'), ord('N'), 27):
            return False


# ---------------------------------------------------------------------------
# Main TUI
# ---------------------------------------------------------------------------

def build_group_rows(local_state, expanded_ids):
    """Flatten groups (and, for expanded ones, their inputs) into one row list.

    Keeping this as a single linear list of ("group", g) / ("input", g, inp)
    tuples lets the groups pane's keyboard navigation (sel_group_idx) treat
    collapsed and expanded groups uniformly.
    """
    rows = []
    for g in local_state.get("groups", []):
        rows.append(("group", g))
        if g.get("id") in expanded_ids:
            for iid in g.get("inputIds", []):
                inp = input_for_id(iid, local_state)
                if inp:
                    rows.append(("input", g, inp))
    return rows


def main(stdscr, port):
    """curses.wrapper entry point: sets up the screen, starts the background
    threads, then loops drawing frames from `state` and handling keypresses.
    """
    global last_error, last_success
    curses.noecho()
    curses.cbreak()
    stdscr.keypad(True)
    curses.curs_set(0)
    stdscr.timeout(50)
    stdscr.clear()
    if curses.has_colors():
        curses.start_color()
        curses.use_default_colors()
        curses.init_pair(SELECTED_PAIR, curses.COLOR_BLACK, curses.COLOR_CYAN)
        curses.init_pair(METER_PAIR, curses.COLOR_YELLOW, -1)
        curses.init_pair(MUTED_PAIR, curses.COLOR_RED, -1)
        curses.init_pair(HEADER_PAIR, curses.COLOR_CYAN, -1)
        curses.init_pair(SUCCESS_PAIR, curses.COLOR_GREEN, -1)
        curses.init_pair(ERROR_PAIR, curses.COLOR_RED, -1)

    pair_cache = {}
    pane = 0  # 0 = groups, 1 = outputs
    sel_group_idx = 0
    sel_output_idx = 0
    group_offset = 0
    output_offset = 0
    expanded_groups = set()

    t_api = threading.Thread(target=api_worker, args=(port,), daemon=True)
    t_api.start()
    t_sse = threading.Thread(target=sse_worker, args=(port,), daemon=True)
    t_sse.start()
    t_job = threading.Thread(target=job_worker, daemon=True)
    t_job.start()
    _route_running.set()
    t_route = threading.Thread(target=_route_worker, args=(port,), daemon=True)
    t_route.start()

    def _startup_restore():
        # Wait briefly for the first SSE state, then restore persisted routes.
        deadline = time.time() + 5
        while time.time() < deadline:
            with state_lock:
                if state.get("groups"):
                    break
            time.sleep(0.05)
        with state_lock:
            _load_tracked_routes(port, state)

    # None of the startup fetches may block the first frame; the UI comes up
    # immediately and fills in as these complete. Applications refresh on
    # their own every 5s (apps_refresh_worker); endpoints only on demand (R).
    threading.Thread(target=_startup_restore, daemon=True).start()
    threading.Thread(target=apps_refresh_worker, args=(port,), daemon=True).start()
    run_job(lambda: fetch_endpoints(port))

    # The most recent status message and when it appeared. Messages are consumed
    # from last_error/last_success immediately but stay on screen for a few
    # seconds -- with the 50 ms frame timeout they would otherwise flash for a
    # single frame and be unreadable.
    msg_text = ""
    msg_is_err = False
    msg_ts = 0.0
    MSG_SECONDS = 4.0

    # Selection is remembered by identity (group/input id, output name), not by
    # row index, so it stays on the same item when an SSE update reorders,
    # inserts, or removes rows.
    sel_group_key = None
    sel_output_key = None

    def row_key(item):
        if item[0] == "group":
            return ("g", item[1].get("id"))
        return ("i", item[1].get("id"), item[2].get("id"))

    # Route-mismatch warnings only show once the mismatch has persisted for a
    # while: Windows briefly reports sessions on the old endpoint during app
    # startup/device switches, which made the warning flicker on and off.
    MISMATCH_GRACE_SECONDS = 8.0
    mismatch_since = {}  # input id -> first time the mismatch was seen

    # Optimistic volumes: rapid +/- presses would otherwise each be computed
    # from the last server-confirmed value (broadcast at most every 200 ms), so
    # three quick presses move the fader once and then it snaps around. The
    # pending value is used as the base for the next nudge and for display
    # until the server catches up (or the entry expires).
    PENDING_SECONDS = 1.5
    pending_group_vol = {}   # group id -> (volume, timestamp)
    pending_out_vol = {}     # output name -> (volume, timestamp)

    def set_group_volume(g, delta=None, absolute=None):
        gid = g.get("id")
        pv = pending_group_vol.get(gid)
        base = pv[0] if pv and time.time() - pv[1] < PENDING_SECONDS else g.get("volume", 100)
        new_vol = clamp_vol(absolute if absolute is not None else base + delta)
        pending_group_vol[gid] = (new_vol, time.time())
        api_call("PATCH", f"/api/groups/{gid}", {"volume": new_vol})

    def set_output_volume(out, delta=None, absolute=None):
        name = out.get("name")
        pv = pending_out_vol.get(name)
        base = pv[0] if pv and time.time() - pv[1] < PENDING_SECONDS else out.get("master", 100)
        new_vol = clamp_vol(absolute if absolute is not None else base + delta)
        pending_out_vol[name] = (new_vol, time.time())
        api_call("POST", "/api/outputs/master", {"name": name, "volume": new_vol})

    while True:
        with state_lock:
            local_state = state
            local_status = status_text
            local_err = last_error
            local_ok = last_success
            local_eps = endpoints
            local_apps = applications
            local_busy = busy_jobs > 0
            last_error = ""
            last_success = ""

        if local_err:
            msg_text, msg_is_err, msg_ts = local_err, True, time.time()
        elif local_ok:
            msg_text, msg_is_err, msg_ts = local_ok, False, time.time()

        # erase() repaints only what changed; clear() would force a full
        # terminal redraw every 50 ms frame and make the whole screen flicker.
        stdscr.erase()
        h, w = stdscr.getmaxyx()

        # Header
        busy = "  |  working..." if local_busy else ""
        header = f" AnniAudio Mixer TUI  |  port {port}  |  {local_status}{busy} "
        try:
            stdscr.addstr(0, 0, header.ljust(w), curses.color_pair(HEADER_PAIR) | curses.A_BOLD)
        except curses.error:
            pass

        # Info line
        if msg_text and time.time() - msg_ts < MSG_SECONDS:
            color = curses.color_pair(ERROR_PAIR) if msg_is_err else curses.color_pair(SUCCESS_PAIR)
            try:
                stdscr.addstr(1, 0, msg_text[:w - 1], color)
            except curses.error:
                pass

        outputs = local_state.get("outputs", [])
        group_rows = build_group_rows(local_state, expanded_groups)
        apps_by_pid = {a.get("processId"): a for a in local_apps}

        # Re-resolve the selection: follow the remembered item if it still
        # exists, otherwise fall back to the nearest valid row index.
        group_keys = [row_key(r) for r in group_rows]
        if sel_group_key in group_keys:
            sel_group_idx = group_keys.index(sel_group_key)
        else:
            sel_group_idx = min(sel_group_idx, max(0, len(group_rows) - 1))
        output_names = [o.get("name") for o in outputs]
        if sel_output_key in output_names:
            sel_output_idx = output_names.index(sel_output_key)
        else:
            sel_output_idx = min(sel_output_idx, max(0, len(outputs) - 1))

        top = 2
        bottom = h - 2
        panel_h = max(1, bottom - top)
        mid = w // 2

        # --- Groups pane ---
        try:
            stdscr.addstr(top, 1, "Virtual Cables", curses.A_BOLD | curses.color_pair(HEADER_PAIR))
            stdscr.hline(top + 1, 1, curses.ACS_HLINE, max(1, mid - 2))
        except curses.error:
            pass

        visible_group = max(0, panel_h - 4)
        if sel_group_idx < group_offset:
            group_offset = sel_group_idx
        if sel_group_idx >= group_offset + visible_group:
            group_offset = sel_group_idx - visible_group + 1

        for i in range(visible_group):
            line = group_offset + i
            if line >= len(group_rows):
                break
            y = top + 2 + i
            item = group_rows[line]
            is_group = item[0] == "group"
            if is_group:
                g = item[1]
                is_sel = pane == 0 and line == sel_group_idx
                gid = g.get("id")
                expanded = gid in expanded_groups
                arrow = "▼" if expanded else "▶"
                name = g.get("name", "?")[:18]
                vol = g.get("volume", 100)
                pv = pending_group_vol.get(gid)
                if pv and time.time() - pv[1] < PENDING_SECONDS:
                    vol = pv[0]
                muted = g.get("muted", False)
                peak = max(g.get("peak", 0.0), g.get("rms", 0.0))
                pct = level_to_pct(peak)
                bar = meter_bar(pct, 8)
                vbar = vol_bar(vol, 10)
                out_list = ", ".join(g.get("outputIds", []))
                color = g.get("color", "#3b82f6")
                attr = group_color_attr(color, pair_cache, selected=is_sel)
                n_inputs = len(g.get("inputIds", []))
                cable = g.get("cable", "")
                mode = "VAC" if cable else "LB"
                input_hint = f" {n_inputs} in [{mode}]"
                content = f"{name:<18} {vbar} {vol:>3.0f}% {bar} {'M' if muted else ' '}"
                content_x = 4
                right_x = content_x + len(content) + 1
                try:
                    stdscr.addstr(y, 1, f"{arrow} ", attr)
                    stdscr.addstr(y, content_x, content, attr)
                    if input_hint:
                        stdscr.addstr(y, right_x, input_hint, attr | curses.A_DIM)
                        right_x += len(input_hint) + 1
                    if out_list:
                        avail = mid - 2 - right_x - 2
                        if avail > 0:
                            stdscr.addstr(y, right_x, f"→ {out_list[:avail]}", attr | curses.A_DIM)
                except curses.error:
                    pass
            else:
                g = item[1]
                inp = item[2]
                is_sel = pane == 0 and line == sel_group_idx
                name = inp.get("name", "?")[:16]
                itype = inp.get("type", "device")[:4]
                peak = max(inp.get("peak", 0.0), inp.get("rms", 0.0))
                pct = level_to_pct(peak)
                bar = meter_bar(pct, 8)
                attr = curses.color_pair(SELECTED_PAIR) if is_sel else curses.A_NORMAL

                # Windows only honors a per-app device change once the app
                # recreates its audio stream, so a route can be configured yet
                # silently not in effect. Compare the app's LIVE session
                # endpoint against the cable this row should be on and say so.
                warn = ""
                iid_key = inp.get("id")
                route = routed_apps.get(iid_key)
                if route:
                    pid, cable, _gid = route
                    live = apps_by_pid.get(pid)
                    if live is None:
                        warn = " (app not running)"
                        mismatch_since.pop(iid_key, None)
                    elif (cable and live.get("isActive")
                          and live.get("endpoint") != cable):
                        # Only warn when the app is ACTIVELY playing into the
                        # wrong endpoint, and only once that has persisted
                        # past the grace period (transient sessions on the old
                        # device are normal during app startup).
                        first = mismatch_since.setdefault(iid_key, time.time())
                        if time.time() - first > MISMATCH_GRACE_SECONDS:
                            warn = f" ! playing on {live.get('endpoint', '?')} - restart app"
                    else:
                        mismatch_since.pop(iid_key, None)
                prefix = f"  • {name:<16} [{itype}] {bar}"
                try:
                    stdscr.addstr(y, 4, prefix, attr)
                    if warn:
                        wattr = curses.A_DIM if warn.startswith(" (") else curses.color_pair(ERROR_PAIR)
                        if is_sel:
                            wattr |= curses.A_REVERSE
                        avail = mid - 2 - (4 + len(prefix))
                        if avail > 0:
                            stdscr.addstr(y, 4 + len(prefix), warn[:avail], wattr)
                except curses.error:
                    pass

        # --- Outputs pane ---
        try:
            stdscr.addstr(top, mid + 1, "Outputs", curses.A_BOLD | curses.color_pair(HEADER_PAIR))
            stdscr.hline(top + 1, mid + 1, curses.ACS_HLINE, max(1, w - mid - 2))
        except curses.error:
            pass

        # Each output occupies two screen rows: the fader line and a dim
        # "<- connected groups" line below it.
        visible_out = max(0, (panel_h - 4) // 2)
        if sel_output_idx < output_offset:
            output_offset = sel_output_idx
        if sel_output_idx >= output_offset + visible_out:
            output_offset = sel_output_idx - visible_out + 1

        for i in range(visible_out):
            line = output_offset + i
            if line >= len(outputs):
                break
            y = top + 2 + i * 2
            out = outputs[line]
            is_sel = pane == 1 and line == sel_output_idx
            name = out.get("name", "?")[:22]
            vol = out.get("master", 100)
            pv = pending_out_vol.get(out.get("name"))
            if pv and time.time() - pv[1] < PENDING_SECONDS:
                vol = pv[0]
            peak = max(out.get("masterPeak", 0.0), out.get("masterRms", 0.0))
            pct = level_to_pct(peak)
            bar = meter_bar(pct, 8)
            vbar = vol_bar(vol, 12)
            attr = curses.color_pair(SELECTED_PAIR) if is_sel else curses.A_NORMAL
            try:
                stdscr.addstr(y, mid + 1, f"{name:<22} {vbar} {vol:>3.0f}% {bar}", attr)
            except curses.error:
                pass
            # connected groups
            gnames = []
            for gid in out.get("groupIds", []):
                g = group_for_id(gid, local_state)
                if g:
                    gnames.append(g.get("name", "?"))
            if gnames:
                try:
                    stdscr.addstr(y + 1, mid + 3, "<- " + ", ".join(gnames)[:w - mid - 6], curses.A_DIM)
                except curses.error:
                    pass

        # Footer
        footer = (
            "Tab:panes  j/k:nav  +/-|v:vol  m:mute  Enter/e:expand  i:add src  a:add app  g:add cable  "
            "C:set cable  o:add output  c:connect  r:rename  d:delete  s:save  q:quit"
        )
        try:
            stdscr.addstr(h - 1, 0, footer[:w - 1], curses.A_DIM)
        except curses.error:
            pass

        stdscr.refresh()

        ch = stdscr.getch()

        # Navigation
        if ch in (9,):
            pane = 1 - pane
        elif ch in (curses.KEY_DOWN, ord('j')):
            if pane == 0:
                sel_group_idx = (sel_group_idx + 1) % max(1, len(group_rows))
            else:
                sel_output_idx = (sel_output_idx + 1) % max(1, len(outputs))
        elif ch in (curses.KEY_UP, ord('k')):
            if pane == 0:
                sel_group_idx = (sel_group_idx - 1) % max(1, len(group_rows))
            else:
                sel_output_idx = (sel_output_idx - 1) % max(1, len(outputs))
        elif ch == curses.KEY_PPAGE:
            if pane == 0:
                sel_group_idx = max(0, sel_group_idx - visible_group)
            else:
                sel_output_idx = max(0, sel_output_idx - visible_out)
        elif ch == curses.KEY_NPAGE:
            if pane == 0:
                sel_group_idx = min(len(group_rows) - 1, sel_group_idx + visible_group)
            else:
                sel_output_idx = min(len(outputs) - 1, sel_output_idx + visible_out)

        # Volume / mute
        elif ch in (ord('+'), ord('='), ord('-')):
            delta = 5 if ch in (ord('+'), ord('=')) else -5
            if pane == 0 and group_rows:
                item = group_rows[sel_group_idx]
                if item[0] == "group":
                    set_group_volume(item[1], delta=delta)
            elif pane == 1 and outputs:
                set_output_volume(outputs[sel_output_idx], delta=delta)
        elif ch == ord('v'):
            if pane == 0 and group_rows:
                item = group_rows[sel_group_idx]
                if item[0] == "group":
                    g = item[1]
                    val = input_dialog(stdscr, "Volume 0-200", str(int(g.get("volume", 100))))
                    if val:
                        try:
                            set_group_volume(g, absolute=float(val))
                        except ValueError:
                            with state_lock:
                                last_error = f"Not a number: {val}"
            elif pane == 1 and outputs:
                out = outputs[sel_output_idx]
                val = input_dialog(stdscr, "Master volume 0-200", str(int(out.get("master", 100))))
                if val:
                    try:
                        set_output_volume(out, absolute=float(val))
                    except ValueError:
                        with state_lock:
                            last_error = f"Not a number: {val}"
        elif ch == ord('m'):
            if pane == 0 and group_rows:
                item = group_rows[sel_group_idx]
                if item[0] == "group":
                    g = item[1]
                    api_call("PATCH", f"/api/groups/{g['id']}", {"muted": not g.get("muted", False)})
        elif ch in (ord('e'), ord('E'), 10, 13, curses.KEY_ENTER):
            if pane == 0 and group_rows:
                item = group_rows[sel_group_idx]
                if item[0] == "group":
                    g = item[1]
                    gid = g.get("id")
                    if gid in expanded_groups:
                        expanded_groups.remove(gid)
                    else:
                        expanded_groups.add(gid)

        # Add / connect / rename / delete / save
        elif ch == ord('i'):
            if not group_rows:
                with state_lock:
                    last_error = "No virtual cable selected"
                continue
            item = group_rows[sel_group_idx]
            g = item[1]
            # One entry per device name, labeled by what capturing it means:
            # VACs expose render+capture under the same name (a cable), real
            # mics are capture-only, speakers/headphones are render-only and
            # get captured via loopback.
            by_name = {}
            for e in local_eps:
                n = e.get("name", "")
                r, c = by_name.get(n, (False, False))
                if e.get("isRender"):
                    r = True
                else:
                    c = True
                by_name[n] = (r, c)
            entries = []
            for n, (r, c) in by_name.items():
                if r and c:
                    kind = "cable"
                elif c:
                    kind = "microphone"
                else:
                    kind = "system audio (loopback)"
                entries.append((n, f"{n}  [{kind}]"))
            idx = list_dialog(stdscr, "Select device source", [lab for _n, lab in entries])
            if idx is None:
                continue
            source = entries[idx][0]
            if source in g.get("outputIds", []):
                if not confirm_dialog(stdscr,
                        f"Feedback loop: this cable already outputs to '{source[:30]}'. Add anyway?"):
                    continue
            val = input_dialog(stdscr, "Source name (optional)", source)
            if val is None:
                continue
            name = val or source
            run_job(lambda g=g, name=name, source=source:
                    _add_input_to_group(port, g, name, "device", source))
        elif ch == ord('a'):
            if not group_rows:
                with state_lock:
                    last_error = "No virtual cable selected"
                continue
            item = group_rows[sel_group_idx]
            g = item[1]
            # Apps only appear here once they own a Windows audio session; a
            # game that has not made a sound yet will be missing. The manual
            # entry covers that (press R after the app starts playing instead).
            # The window title is shown purely to help identify oddly named
            # exes; the exe/pid is still what gets routed.
            def app_label(a):
                name = a.get('name', '?')
                title = (a.get('windowTitle') or '').strip()
                ep = a.get('endpoint', '?')
                if title and title.lower() != name.lower():
                    return f"{name} - \"{title[:48]}\" ({ep})"
                return f"{name} ({ep})"

            app_labels = [app_label(a) for a in local_apps]
            app_labels.append("<app not listed - enter PID manually>")
            idx = list_dialog(stdscr, "Select application", app_labels)
            if idx is None:
                continue
            if idx == len(local_apps):
                val = input_dialog(stdscr, "Process ID (see Task Manager > Details)")
                if not val:
                    continue
                try:
                    pid = int(val)
                except ValueError:
                    with state_lock:
                        last_error = f"Not a valid PID: {val}"
                    continue
                app = {"processId": pid, "name": f"pid {pid}"}
            else:
                app = local_apps[idx]
            run_job(lambda g=g, app=app: _assign_app_to_group(port, g, app))
        elif ch == ord('g'):
            val = input_dialog(stdscr, "New virtual cable name")
            if val:
                api_call("POST", "/api/groups", {"name": val, "color": "#3b82f6"})
        elif ch == ord('o'):
            renders = [e.get("name", "") for e in local_eps if e.get("isRender")]
            idx = list_dialog(stdscr, "Select output endpoint", renders)
            if idx is None:
                continue
            api_call("POST", "/api/outputs", {"name": renders[idx]})
        elif ch == ord('c'):
            if not group_rows or not outputs:
                with state_lock:
                    last_error = "Need a virtual cable and an output"
                continue
            g_item = group_rows[sel_group_idx]
            if g_item[0] != "group":
                with state_lock:
                    last_error = "Select a virtual cable header"
                continue
            g = g_item[1]
            out = outputs[sel_output_idx]
            outs = list(g.get("outputIds", []))
            connected = out["name"] in outs
            if not connected:
                sources = set()
                for iid in g.get("inputIds", []):
                    inp = input_for_id(iid, local_state)
                    if inp:
                        sources.add(inp.get("source"))
                if out["name"] in sources:
                    if not confirm_dialog(stdscr,
                            f"Feedback loop: this cable captures '{out['name'][:30]}'. Connect anyway?"):
                        continue
            if connected:
                outs.remove(out["name"])
            else:
                outs.append(out["name"])
            api_call("PATCH", f"/api/groups/{g['id']}", {"outputIds": outs})
            with state_lock:
                if "Virtual Audio Cable" in out.get("name", "") or out.get("isAnniAudio"):
                    if not connected:
                        last_success = f"Group now sends audio to VAC {out['name']} (silent until another app captures it)"
                    else:
                        last_success = f"Disconnected from VAC {out['name']}"
                else:
                    last_success = f"{'Disconnected from' if connected else 'Connected to'} {out['name']}"
        elif ch == ord('C'):
            if pane == 0 and group_rows:
                item = group_rows[sel_group_idx]
                if item[0] == "group":
                    g = item[1]
                    # Only render endpoints make sense as a group cable.
                    renders = [e.get("name", "") for e in local_eps if e.get("isRender")]
                    labels = ["(none - loopback capture)"] + renders
                    idx = list_dialog(stdscr, "Select group cable", labels)
                    if idx is None:
                        continue
                    cable = "" if idx == 0 else renders[idx - 1]
                    run_job(lambda g=g, cable=cable: _set_group_cable(port, g, cable))
        elif ch == ord('r'):
            if pane == 0 and group_rows:
                item = group_rows[sel_group_idx]
                if item[0] == "group":
                    g = item[1]
                    val = input_dialog(stdscr, "Rename virtual cable", g.get("name", ""))
                    if val:
                        api_call("PATCH", f"/api/groups/{g['id']}", {"name": val})
        elif ch == ord('d'):
            def input_used_elsewhere(iid, exclude_gid):
                for g2 in local_state.get("groups", []):
                    if g2.get("id") != exclude_gid and iid in g2.get("inputIds", []):
                        return True
                return False

            if pane == 0 and group_rows:
                item = group_rows[sel_group_idx]
                if item[0] == "group":
                    g = item[1]
                    if confirm_dialog(stdscr, f"Delete virtual cable '{g.get('name')}'?"):
                        for iid in g.get("inputIds", []):
                            if iid in routed_apps:
                                pid, _cable, _gid = routed_apps.pop(iid)
                                run_job(lambda pid=pid: _clear_app_route(pid))
                            # Delete inputs that only this group used, so they
                            # do not linger in the config as orphans.
                            if not input_used_elsewhere(iid, g.get("id")):
                                api_call("DELETE", f"/api/inputs/{iid}")
                        api_call("DELETE", f"/api/groups/{g['id']}")
                else:
                    g = item[1]
                    inp = item[2]
                    iid = inp["id"]
                    if iid in routed_apps:
                        pid, _cable, _gid = routed_apps.pop(iid)
                        run_job(lambda pid=pid: _clear_app_route(pid))
                    ids = [x for x in g.get("inputIds", []) if x != iid]
                    api_call("PATCH", f"/api/groups/{g['id']}", {"inputIds": ids})
                    if not input_used_elsewhere(iid, g.get("id")):
                        api_call("DELETE", f"/api/inputs/{iid}")
            elif pane == 1 and outputs:
                out = outputs[sel_output_idx]
                if confirm_dialog(stdscr, f"Delete output '{out.get('name')}'?"):
                    api_call("DELETE", "/api/outputs", {"name": out["name"]})
        elif ch == ord('s'):
            val = input_dialog(stdscr, "Save path (empty = autosave)", "")
            if val is not None:
                api_call("POST", "/api/presets/save", {"path": val})
        elif ch == ord('R'):
            run_job(lambda: fetch_endpoints(port))
            run_job(lambda: fetch_applications(port))
        elif ch in (ord('q'), ord('Q')):
            break

        # Remember the (possibly moved) selection by identity for the next frame.
        sel_group_key = group_keys[sel_group_idx] if group_keys else None
        sel_output_key = output_names[sel_output_idx] if output_names else None

    with state_lock:
        _save_tracked_routes(port, state)
    _route_running.clear()
    api_queue.put(None)
    job_queue.put(None)


def run():
    """CLI entry point: parse args, then hand off to curses.wrapper (which
    restores the terminal on exit or crash) so main() can own the screen.
    """
    parser = argparse.ArgumentParser(
        description="AnniAudio mixer TUI - curses front-end for the mixer control API.")
    parser.add_argument("--port", type=int, default=8850,
                         help="mixer control API port (must match the running route_cli mixer instance)")
    args = parser.parse_args()
    if not sys.stdin.isatty():
        print("mixer_tui.py must be run in an interactive terminal.")
        sys.exit(1)
    try:
        curses.wrapper(lambda stdscr: main(stdscr, args.port))
    except Exception:
        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    run()
