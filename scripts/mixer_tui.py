#!/usr/bin/env python3
"""
AnniAudio mixer TUI.

Connects to the mixer control API on 127.0.0.1 and gives a compact, live-updating
terminal interface for outputs, routes, levels, volume, mute, and preset save.

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
import urllib.parse

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
state = {"running": False, "outputs": []}
endpoints = []
status_text = "Connecting..."
last_error = ""
last_success = ""

api_queue = queue.Queue()

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
            kwargs = {"timeout": 15}
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
            r = session.get(base + "/api/events", stream=True, timeout=20)
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
        r = requests.get(BASE.format(port=port) + "/api/endpoints", timeout=5)
        if r.ok:
            with state_lock:
                endpoints = r.json().get("endpoints", [])
    except Exception as e:
        with state_lock:
            endpoints = []

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


def vol_bar(vol, width=20):
    filled = int(round(vol / 200 * width))
    return "█" * filled + "░" * (width - filled)


def clamp_vol(v):
    return max(0, min(200, v))

# ---------------------------------------------------------------------------
# Dialogs
# ---------------------------------------------------------------------------

def draw_box(win, title=""):
    h, w = win.getmaxyx()
    win.box()
    if title:
        win.addstr(0, 2, f" {title} ", curses.A_BOLD)


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
                attr = curses.color_pair(2) | curses.A_BOLD
            win.addstr(2 + i, 2, str(items[line])[:max_w - 4], attr)
        win.refresh()
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
        win.addstr(2, 2, text[:width - 4])
        win.refresh()
        ch = win.getch()
        if ch in (curses.KEY_ENTER, 10, 13):
            return text
        elif ch in (27,):
            return None
        elif ch == curses.KEY_BACKSPACE or ch == 127 or ch == 8:
            text = text[:-1]
        elif 32 <= ch <= 126:
            text += chr(ch)

# ---------------------------------------------------------------------------
# Main TUI
# ---------------------------------------------------------------------------

def main(stdscr, port):
    global last_error, last_success, state, endpoints, status_text
    curses.curs_set(0)
    stdscr.timeout(50)
    stdscr.clear()
    if curses.has_colors():
        curses.start_color()
        curses.use_default_colors()
        curses.init_pair(1, curses.COLOR_CYAN, -1)      # header
        curses.init_pair(2, curses.COLOR_BLACK, curses.COLOR_CYAN)  # selected
        curses.init_pair(3, curses.COLOR_YELLOW, -1)    # meter
        curses.init_pair(4, curses.COLOR_RED, -1)       # muted/error
        curses.init_pair(5, curses.COLOR_GREEN, -1)      # success

    pane = 0  # 0 = outputs, 1 = routes
    sel_out = 0
    sel_route = 0

    # start workers
    t_api = threading.Thread(target=api_worker, args=(port,), daemon=True)
    t_api.start()
    t_sse = threading.Thread(target=sse_worker, args=(port,), daemon=True)
    t_sse.start()
    fetch_endpoints(port)

    while True:
        with state_lock:
            local_state = state
            local_status = status_text
            local_err = last_error
            local_ok = last_success
            local_eps = endpoints

        # Clear and resize handling
        stdscr.clear()
        h, w = stdscr.getmaxyx()

        # Header
        header = f" AnniAudio Mixer TUI  |  port {port}  |  {local_status} "
        stdscr.addstr(0, 0, header.ljust(w), curses.color_pair(1) | curses.A_BOLD)

        # Error / success line
        info = local_err or local_ok
        if info:
            color = curses.color_pair(4) if local_err else curses.color_pair(5)
            stdscr.addstr(1, 0, info[:w - 1], color)
            if local_err or local_ok:
                with state_lock:
                    if local_err:
                        last_error = ""
                    if local_ok:
                        last_success = ""

        outputs = local_state.get("outputs", [])
        routes = []
        for out in outputs:
            for r in out.get("strips", []):
                routes.append(r)

        # Panel dimensions
        top = 2
        mid = w // 2
        bottom = h - 2
        panel_h = bottom - top

        # Outputs panel
        stdscr.addstr(top, 1, "Outputs", curses.A_BOLD | curses.color_pair(1))
        stdscr.hline(top + 1, 1, curses.ACS_HLINE, max(1, mid - 2))
        out_count = max(0, panel_h - 3)
        for i, out in enumerate(outputs[:out_count]):
            y = top + 2 + i
            is_sel = pane == 0 and i == sel_out
            attr = curses.color_pair(2) if is_sel else curses.A_NORMAL
            name = out.get("name", "?")[:mid - 18]
            vol = out.get("master", 100)
            peak = out.get("masterPeak", 0)
            rms = out.get("masterRms", 0)
            pct = dbm_percent(max(peak, rms))
            bar = meter_bar(pct, 8)
            line = f"{name[:mid-18]:<{mid-18}} {bar} {vol:>3.0f}%"
            stdscr.addstr(y, 1, line[:mid - 2], attr)

        # Routes panel
        stdscr.addstr(top, mid + 1, "Routes", curses.A_BOLD | curses.color_pair(1))
        stdscr.hline(top + 1, mid + 1, curses.ACS_HLINE, max(1, mid - 2))
        route_count = max(0, panel_h - 3)
        for i, r in enumerate(routes[:route_count]):
            y = top + 2 + i
            is_sel = pane == 1 and i == sel_route
            attr = curses.color_pair(2) if is_sel else curses.A_NORMAL
            name = r.get("name", r.get("source", "?"))[:20]
            outname = r.get("output", "?")[:14]
            vol = r.get("volume", 100)
            muted = r.get("muted", False)
            peak = r.get("peak", 0)
            rms = r.get("rms", 0)
            pct = dbm_percent(max(peak, rms))
            bar = meter_bar(pct, 8)
            text = f"{name:<20} -> {outname:<14} {bar} {vol:>3.0f}% {'M' if muted else ''}"
            stdscr.addstr(y, mid + 1, text[:mid - 2], attr)

        # Footer help
        footer = (
            "Tab/Arrows:nav  +/-|v:vol  m:mute  r:rename  d:delete  "
            "a:add route  o:add output  s:save  R:refresh  q:quit"
        )
        stdscr.addstr(h - 1, 0, footer[:w - 1])

        stdscr.refresh()

        ch = stdscr.getch()

        # Navigation
        if ch in (9,):
            pane = 1 - pane
        elif ch in (curses.KEY_DOWN, ord('j')):
            if pane == 0:
                sel_out = (sel_out + 1) % max(1, len(outputs))
            else:
                sel_route = (sel_route + 1) % max(1, len(routes))
        elif ch in (curses.KEY_UP, ord('k')):
            if pane == 0:
                sel_out = (sel_out - 1) % max(1, len(outputs))
            else:
                sel_route = (sel_route - 1) % max(1, len(routes))
        elif ch in (ord('+'), ord('=')):
            if pane == 0 and outputs:
                out = outputs[sel_out]
                new_vol = clamp_vol(out.get("master", 100) + 5)
                api_call("POST", "/api/outputs/master", {"name": out["name"], "volume": new_vol})
            elif pane == 1 and routes:
                r = routes[sel_route]
                new_vol = clamp_vol(r.get("volume", 100) + 5)
                api_call("PATCH", f"/api/strips/{r['id']}", {"volume": new_vol})
        elif ch == ord('-'):
            if pane == 0 and outputs:
                out = outputs[sel_out]
                new_vol = clamp_vol(out.get("master", 100) - 5)
                api_call("POST", "/api/outputs/master", {"name": out["name"], "volume": new_vol})
            elif pane == 1 and routes:
                r = routes[sel_route]
                new_vol = clamp_vol(r.get("volume", 100) - 5)
                api_call("PATCH", f"/api/strips/{r['id']}", {"volume": new_vol})
        elif ch == ord('v'):
            if pane == 1 and routes:
                r = routes[sel_route]
                val = input_dialog(stdscr, "Volume 0-200", str(int(r.get("volume", 100))))
                if val:
                    try:
                        new_vol = clamp_vol(float(val))
                        api_call("PATCH", f"/api/strips/{r['id']}", {"volume": new_vol})
                    except Exception:
                        pass
            elif pane == 0 and outputs:
                out = outputs[sel_out]
                val = input_dialog(stdscr, "Master volume 0-200", str(int(out.get("master", 100))))
                if val:
                    try:
                        new_vol = clamp_vol(float(val))
                        api_call("POST", "/api/outputs/master", {"name": out["name"], "volume": new_vol})
                    except Exception:
                        pass
        elif ch == ord('m'):
            if pane == 1 and routes:
                r = routes[sel_route]
                api_call("PATCH", f"/api/strips/{r['id']}", {"muted": not r.get("muted", False)})
        elif ch == ord('r'):
            if pane == 1 and routes:
                r = routes[sel_route]
                val = input_dialog(stdscr, "Rename", r.get("name", ""))
                if val is not None:
                    api_call("PATCH", f"/api/strips/{r['id']}", {"name": val})
        elif ch == ord('d'):
            if pane == 0 and outputs:
                out = outputs[sel_out]
                api_call("DELETE", "/api/outputs", {"name": out["name"]})
            elif pane == 1 and routes:
                r = routes[sel_route]
                api_call("DELETE", f"/api/strips/{r['id']}")
        elif ch == ord('a'):
            sources = [e["name"] for e in local_eps]
            out_names = [o["name"] for o in outputs]
            if not sources or not out_names:
                with state_lock:
                    last_error = "Need at least one endpoint and one output"
                continue
            src_idx = list_dialog(stdscr, "Select source", sources)
            if src_idx is None:
                continue
            out_idx = list_dialog(stdscr, "Select output", out_names)
            if out_idx is None:
                continue
            name = input_dialog(stdscr, "Name (optional)", sources[src_idx])
            if name is None:
                continue
            vol = input_dialog(stdscr, "Volume 0-200", "100")
            try:
                vol = clamp_vol(float(vol))
            except Exception:
                vol = 100
            api_call("POST", "/api/strips", {
                "source": sources[src_idx],
                "output": out_names[out_idx],
                "name": name,
                "volume": vol
            })
        elif ch == ord('o'):
            renders = [e["name"] for e in local_eps if e.get("isRender")]
            if not renders:
                with state_lock:
                    last_error = "No render endpoints available"
                continue
            idx = list_dialog(stdscr, "Select output endpoint", renders)
            if idx is None:
                continue
            api_call("POST", "/api/outputs", {"name": renders[idx]})
        elif ch == ord('s'):
            path = input_dialog(stdscr, "Save path", "config/mixers/")
            if path:
                api_call("POST", "/api/presets/save", {"path": path})
        elif ch == ord('R'):
            fetch_endpoints(port)
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
