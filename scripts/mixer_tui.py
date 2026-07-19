#!/usr/bin/env python3
"""
AnniAudio mixer TUI (inputs -> groups -> outputs).

Connects to the mixer control API on 127.0.0.1 and gives a compact, live-updating
terminal interface for groups, inputs, outputs, levels, volume, mute, and color-coded
cables.

Usage:
    python scripts/mixer_tui.py [--port 8850]

Requires the route_cli mixer to already be running on the same port.
"""

import argparse
import json
import math
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

BASE = "http://127.0.0.1:{port}"

# Shared state protected by lock
state_lock = threading.Lock()
state = {"running": False, "inputs": [], "groups": [], "outputs": []}
endpoints = []
applications = []
status_text = "Connecting..."
last_error = ""
last_success = ""

api_queue = queue.Queue()

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
    api_queue.put((method, path, body))


# ---------------------------------------------------------------------------
# SSE / polling worker
# ---------------------------------------------------------------------------

def sse_worker(port):
    global status_text
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
                            global state
                            state = payload
                    except Exception as e:
                        with state_lock:
                            status_text = f"SSE parse error: {e}"
        except Exception as e:
            with state_lock:
                status_text = f"SSE error: {e}"
            time.sleep(2)


def fetch_endpoints(port):
    global endpoints
    try:
        r = requests.get(BASE.format(port=port) + "/api/endpoints", timeout=15)
        if r.ok:
            with state_lock:
                endpoints = r.json().get("endpoints", [])
    except Exception as e:
        with state_lock:
            endpoints = []


def fetch_applications(port):
    global applications
    try:
        r = requests.get(BASE.format(port=port) + "/api/applications", timeout=15)
        if r.ok:
            with state_lock:
                applications = r.json().get("applications", [])
    except Exception:
        with state_lock:
            applications = []


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def dbm_percent(v):
    if v <= 0.00001:
        return 0
    db = 20 * math.log10(min(v, 1.0))
    return max(0, min(100, (db + 60) / 60 * 100))


def meter_bar(pct, width=10):
    filled = int(round(pct / 100 * width))
    return "█" * filled + "░" * (width - filled)


def vol_bar(vol, width=16):
    filled = int(round(vol / 200 * width))
    return "█" * filled + "░" * (width - filled)


def clamp_vol(v):
    return max(0, min(200, v))


def hex_to_curses_color(hexstr):
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
    for inp in local_state.get("inputs", []):
        if inp.get("id") == iid:
            return inp
    return None


def group_for_id(gid, local_state):
    for g in local_state.get("groups", []):
        if g.get("id") == gid:
            return g
    return None


# ---------------------------------------------------------------------------
# Dialogs
# ---------------------------------------------------------------------------

def draw_box(win, title=""):
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
    if not items:
        return None
    h, w = stdscr.getmaxyx()
    max_h = min(h - 4, max(10, len(items) + 4))
    max_w = min(w - 4, max(40, max(len(str(x)) for x in items) + 8))
    y = (h - max_h) // 2
    x = (w - max_w) // 2
    win = curses.newwin(max_h, max_w, y, x)
    win.keypad(True)
    idx = start_idx
    while True:
        win.erase()
        draw_box(win, title)
        visible = max_h - 4
        offset = max(0, min(idx, len(items) - visible))
        for i in range(visible):
            line = offset + i
            if line >= len(items):
                break
            attr = curses.A_NORMAL
            if line == idx:
                attr = curses.color_pair(SELECTED_PAIR) | curses.A_BOLD
            try:
                win.addstr(2 + i, 2, str(items[line])[:max_w - 4], attr)
            except curses.error:
                pass
        try:
            win.refresh()
        except curses.error:
            pass
        ch = win.getch()
        if ch in (curses.KEY_UP, ord('k')):
            idx = (idx - 1) % len(items)
        elif ch in (curses.KEY_DOWN, ord('j')):
            idx = (idx + 1) % len(items)
        elif ch in (curses.KEY_ENTER, 10, 13):
            return idx
        elif ch in (27, ord('q'), ord('Q')):
            return None


def input_dialog(stdscr, title, default=""):
    h, w = stdscr.getmaxyx()
    height = 5
    width = min(w - 4, max(50, len(title) + 10))
    y = (h - height) // 2
    x = (w - width) // 2
    win = curses.newwin(height, width, y, x)
    win.keypad(True)
    text = default
    while True:
        win.erase()
        draw_box(win, title)
        try:
            win.addstr(2, 2, text[:width - 4])
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


def confirm_dialog(stdscr, text):
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
    rows = []
    for g in local_state.get("groups", []):
        rows.append(("group", g))
        if g.get("id") in expanded_ids:
            for iid in g.get("inputIds", []):
                inp = input_for_id(iid, local_state)
                if inp:
                    rows.append(("input", g, inp))
    return rows


def selected_group(rows, sel_idx):
    if not rows or sel_idx < 0 or sel_idx >= len(rows):
        return None
    item = rows[sel_idx]
    return item[1]


def selected_input(rows, sel_idx):
    if not rows or sel_idx < 0 or sel_idx >= len(rows):
        return None
    item = rows[sel_idx]
    if item[0] == "input":
        return item[2]
    return None


def main(stdscr, port):
    global last_error, last_success, state, endpoints, applications, status_text
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
    fetch_endpoints(port)
    fetch_applications(port)

    while True:
        with state_lock:
            local_state = state
            local_status = status_text
            local_err = last_error
            local_ok = last_success
            local_eps = endpoints

        stdscr.clear()
        h, w = stdscr.getmaxyx()

        # Header
        header = f" AnniAudio Mixer TUI  |  port {port}  |  {local_status} "
        try:
            stdscr.addstr(0, 0, header.ljust(w), curses.color_pair(HEADER_PAIR) | curses.A_BOLD)
        except curses.error:
            pass

        # Info line
        info = local_err or local_ok
        if info:
            color = curses.color_pair(ERROR_PAIR) if local_err else curses.color_pair(SUCCESS_PAIR)
            try:
                stdscr.addstr(1, 0, info[:w - 1], color)
            except curses.error:
                pass
            if local_err or local_ok:
                with state_lock:
                    if local_err:
                        last_error = ""
                    if local_ok:
                        last_success = ""

        groups = local_state.get("groups", [])
        outputs = local_state.get("outputs", [])
        group_rows = build_group_rows(local_state, expanded_groups)

        if sel_group_idx >= len(group_rows):
            sel_group_idx = max(0, len(group_rows) - 1)
        if sel_output_idx >= len(outputs):
            sel_output_idx = max(0, len(outputs) - 1)

        top = 2
        bottom = h - 2
        panel_h = max(1, bottom - top)
        mid = w // 2

        # --- Groups pane ---
        try:
            stdscr.addstr(top, 1, "Groups", curses.A_BOLD | curses.color_pair(HEADER_PAIR))
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
                muted = g.get("muted", False)
                peak = max(g.get("peak", 0.0), g.get("rms", 0.0))
                pct = dbm_percent(peak)
                bar = meter_bar(pct, 8)
                vbar = vol_bar(vol, 10)
                out_list = ", ".join(g.get("outputIds", []))[:mid - 50]
                color = g.get("color", "#3b82f6")
                attr = group_color_attr(color, pair_cache, selected=is_sel)
                n_inputs = len(g.get("inputIds", []))
                input_hint = f" {n_inputs} in" if n_inputs > 1 else ""
                try:
                    stdscr.addstr(y, 1, f"{arrow} ", attr)
                    stdscr.addstr(y, 4, f"{name:<18} {vbar} {vol:>3.0f}% {bar} {'M' if muted else ' '}", attr)
                    if out_list:
                        stdscr.addstr(y, mid - 2 - len(out_list), f"→ {out_list}", attr | curses.A_DIM)
                    if input_hint:
                        stdscr.addstr(y, mid - 10, input_hint, attr | curses.A_DIM)
                except curses.error:
                    pass
            else:
                g = item[1]
                inp = item[2]
                is_sel = pane == 0 and line == sel_group_idx
                name = inp.get("name", "?")[:16]
                itype = inp.get("type", "device")[:4]
                peak = max(inp.get("peak", 0.0), inp.get("rms", 0.0))
                pct = dbm_percent(peak)
                bar = meter_bar(pct, 8)
                attr = curses.color_pair(SELECTED_PAIR) if is_sel else curses.A_NORMAL
                try:
                    stdscr.addstr(y, 4, f"  • {name:<16} [{itype}] {bar}", attr)
                except curses.error:
                    pass

        # --- Outputs pane ---
        try:
            stdscr.addstr(top, mid + 1, "Outputs", curses.A_BOLD | curses.color_pair(HEADER_PAIR))
            stdscr.hline(top + 1, mid + 1, curses.ACS_HLINE, max(1, w - mid - 2))
        except curses.error:
            pass

        visible_out = max(0, panel_h - 4)
        if sel_output_idx < output_offset:
            output_offset = sel_output_idx
        if sel_output_idx >= output_offset + visible_out:
            output_offset = sel_output_idx - visible_out + 1

        for i in range(visible_out):
            line = output_offset + i
            if line >= len(outputs):
                break
            y = top + 2 + i
            out = outputs[line]
            is_sel = pane == 1 and line == sel_output_idx
            name = out.get("name", "?")[:22]
            vol = out.get("master", 100)
            peak = max(out.get("masterPeak", 0.0), out.get("masterRms", 0.0))
            pct = dbm_percent(peak)
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
            "Tab:panes  j/k:nav  +/-|v:vol  m:mute  Enter/e:expand  i:add input  g:add group  "
            "o:add output  c:connect  r:rename  d:delete  s:save  q:quit"
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
        elif ch in (ord('+'), ord('=')):
            if pane == 0 and group_rows:
                item = group_rows[sel_group_idx]
                if item[0] == "group":
                    g = item[1]
                    new_vol = clamp_vol(g.get("volume", 100) + 5)
                    api_call("PATCH", f"/api/groups/{g['id']}", {"volume": new_vol})
            elif pane == 1 and outputs:
                out = outputs[sel_output_idx]
                new_vol = clamp_vol(out.get("master", 100) + 5)
                api_call("POST", "/api/outputs/master", {"name": out["name"], "volume": new_vol})
        elif ch == ord('-'):
            if pane == 0 and group_rows:
                item = group_rows[sel_group_idx]
                if item[0] == "group":
                    g = item[1]
                    new_vol = clamp_vol(g.get("volume", 100) - 5)
                    api_call("PATCH", f"/api/groups/{g['id']}", {"volume": new_vol})
            elif pane == 1 and outputs:
                out = outputs[sel_output_idx]
                new_vol = clamp_vol(out.get("master", 100) - 5)
                api_call("POST", "/api/outputs/master", {"name": out["name"], "volume": new_vol})
        elif ch == ord('v'):
            if pane == 0 and group_rows:
                item = group_rows[sel_group_idx]
                if item[0] == "group":
                    g = item[1]
                    val = input_dialog(stdscr, "Volume 0-200", str(int(g.get("volume", 100))))
                    if val:
                        try:
                            new_vol = clamp_vol(float(val))
                            api_call("PATCH", f"/api/groups/{g['id']}", {"volume": new_vol})
                        except Exception:
                            pass
            elif pane == 1 and outputs:
                out = outputs[sel_output_idx]
                val = input_dialog(stdscr, "Master volume 0-200", str(int(out.get("master", 100))))
                if val:
                    try:
                        new_vol = clamp_vol(float(val))
                        api_call("POST", "/api/outputs/master", {"name": out["name"], "volume": new_vol})
                    except Exception:
                        pass
        elif ch == ord('m'):
            if pane == 0 and group_rows:
                item = group_rows[sel_group_idx]
                if item[0] == "group":
                    g = item[1]
                    api_call("PATCH", f"/api/groups/{g['id']}", {"muted": not g.get("muted", False)})

        elif ch in (ord('e'), ord('E'), 10, curses.KEY_ENTER):
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
                    last_error = "No group selected"
                continue
            item = group_rows[sel_group_idx]
            g = item[1]
            names = [e.get("name", "") for e in local_eps]
            idx = list_dialog(stdscr, "Select device input", names)
            if idx is None:
                continue
            source = names[idx]
            val = input_dialog(stdscr, "Input name (optional)", source)
            if val is None:
                continue
            name = val or source
            # add input, then add to group (input add is cheap; the route rebuild is async)
            try:
                r = requests.post(BASE.format(port=port) + "/api/inputs",
                                  json={"name": name, "type": "device", "source": source}, timeout=15)
                if r.ok:
                    inp = r.json()
                    iid = inp.get("id")
                    if iid is not None:
                        ids = list(g.get("inputIds", []))
                        if iid not in ids:
                            ids.append(iid)
                            api_call("PATCH", f"/api/groups/{g['id']}", {"inputIds": ids})
                else:
                    with state_lock:
                        try:
                            last_error = r.json().get("error", str(r.status_code))
                        except Exception:
                            last_error = str(r.status_code)
            except Exception as e:
                with state_lock:
                    last_error = str(e)
        elif ch == ord('g'):
            val = input_dialog(stdscr, "New group name")
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
                    last_error = "Need a group and an output"
                continue
            g_item = group_rows[sel_group_idx]
            if g_item[0] != "group":
                with state_lock:
                    last_error = "Select a group header"
                continue
            g = g_item[1]
            out = outputs[sel_output_idx]
            outs = list(g.get("outputIds", []))
            if out["name"] in outs:
                outs.remove(out["name"])
            else:
                outs.append(out["name"])
            api_call("PATCH", f"/api/groups/{g['id']}", {"outputIds": outs})
        elif ch == ord('r'):
            if pane == 0 and group_rows:
                item = group_rows[sel_group_idx]
                if item[0] == "group":
                    g = item[1]
                    val = input_dialog(stdscr, "Rename group", g.get("name", ""))
                    if val:
                        api_call("PATCH", f"/api/groups/{g['id']}", {"name": val})
        elif ch == ord('d'):
            if pane == 0 and group_rows:
                item = group_rows[sel_group_idx]
                if item[0] == "group":
                    g = item[1]
                    if confirm_dialog(stdscr, f"Delete group '{g.get('name')}'?"):
                        api_call("DELETE", f"/api/groups/{g['id']}")
                else:
                    g = item[1]
                    inp = item[2]
                    ids = [x for x in g.get("inputIds", []) if x != inp["id"]]
                    api_call("PATCH", f"/api/groups/{g['id']}", {"inputIds": ids})
            elif pane == 1 and outputs:
                out = outputs[sel_output_idx]
                if confirm_dialog(stdscr, f"Delete output '{out.get('name')}'?"):
                    api_call("DELETE", "/api/outputs", {"name": out["name"]})
        elif ch == ord('s'):
            val = input_dialog(stdscr, "Save path (empty = autosave)", "")
            if val is not None:
                api_call("POST", "/api/presets/save", {"path": val})
        elif ch == ord('R'):
            fetch_endpoints(port)
            fetch_applications(port)
        elif ch in (ord('q'), ord('Q'), 27):
            break

    api_queue.put(None)


def run():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8850)
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
