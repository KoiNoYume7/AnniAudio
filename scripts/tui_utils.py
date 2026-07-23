"""Pure helper functions for the AnniAudio mixer TUI."""
import curses
import math

SELECTED_PAIR = 1
METER_PAIR = 2
MUTED_PAIR = 3
HEADER_PAIR = 4
SUCCESS_PAIR = 5
ERROR_PAIR = 6
GROUP_PAIR_START = 10

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


def app_label(a):
    """Picker label for an application: exe, window title (id helper), endpoint."""
    name = a.get('name', '?')
    title = (a.get('windowTitle') or '').strip()
    ep = a.get('endpoint', '?')
    if title and title.lower() != name.lower():
        return f"{name} - \"{title[:48]}\" ({ep})"
    return f"{name} ({ep})"


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


