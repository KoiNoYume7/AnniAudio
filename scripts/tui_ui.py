"""Curses dialog and row-building helpers for the AnniAudio mixer TUI."""
import curses

from tui_utils import SELECTED_PAIR, input_for_id

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

def short_cable(name):
    """Trim the boilerplate off a VAC endpoint name for display."""
    return (name or "").replace(" (Virtual Audio Cable)", "").strip() or name


def build_group_rows(local_state, expanded_ids, apps_by_endpoint=None):
    """Flatten groups (and, for expanded ones, their inputs and live app members)
    into one linear row list of ("group", g) / ("input", g, inp) / ("app", g, app)
    tuples, so the groups pane's keyboard navigation treats them uniformly.

    For a cabled group the "app" rows come straight from which applications are
    *actually* playing into that VAC right now (their live session endpoint), so
    the membership is always truthful and self-heals when an app is re-routed --
    no client-side tracking involved.
    """
    apps_by_endpoint = apps_by_endpoint or {}
    rows = []
    for g in local_state.get("groups", []):
        rows.append(("group", g))
        if g.get("id") in expanded_ids:
            for iid in g.get("inputIds", []):
                inp = input_for_id(iid, local_state)
                if inp:
                    rows.append(("input", g, inp))
            cable = g.get("cable", "")
            if cable:
                for app in apps_by_endpoint.get(cable, []):
                    rows.append(("app", g, app))
    return rows


