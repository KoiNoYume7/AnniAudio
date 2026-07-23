# AnniAudio Mixer TUI

`scripts/mixer_tui.py` is a curses terminal front-end for the mixer control API
(see `docs/MIXER-CONTROL-API.md`). It is a thin REST/SSE client — it never
touches audio directly, so it can be closed and reopened without affecting
playback, and it stays in sync with any other connected client.

## Launch

```powershell
# Use the batch wrapper (also installs windows-curses / winappaudiorouter if missing)
.\mixer-tui.bat [PORT]

# Or run directly
python scripts\mixer_tui.py [--port 8850] [--repair-routes]
```

- `PORT` defaults to `8850`.
- `--repair-routes` makes the TUI continuously rewrite per-app routes to match
  the mixer state. It is **off by default** because some apps (e.g. Spotify)
  reassert their own output device and fight the repair loop, causing the route
  to flip back and forth. Enable it only if you want aggressive repair.

## Panes

The screen is split into two panes:

- **Virtual Cables** (left) — the mixer groups/cables, each with volume, mute,
  colour, and optionally expanded inputs/apps.
- **Outputs** (right) — physical render devices with master volume/mute.

`Tab` switches between panes. The selected item is highlighted.

## Global keys

| Key | Action |
|---|---|
| `Tab` | Switch between Virtual Cables and Outputs panes |
| `j`/`k`, `↑`/`↓` | Move selection |
| `PgUp`/`PgDn` | Page selection up/down |
| `+`/`-` | Nudge volume by 5 % |
| `v` | Type an exact volume (0–200 %) |
| `m` | Toggle mute on selected cable/output |
| `R` | Refresh endpoint and application lists |
| `q` | Quit (`Esc` only cancels dialogs; pickers support type-to-filter) |

## Virtual cable keys

| Key | Action |
|---|---|
| `Enter`/`e`/`E` | Expand/collapse the selected cable |
| `i` | Add a device source to the selected cable |
| `a` | Add a running application to the selected cable |
| `g` | Add a new virtual cable |
| `C` | Set the selected cable's group cable (render VAC) for per-app routing |
| `c` | Connect/disconnect selected cable to/from selected output |
| `x` | Set the selected cable's send level to one of its outputs |
| `r` | Rename selected cable |
| `d` | Delete selected cable/input/app/output |
| `S` | Scenes: apply a saved level overlay or save current levels as a new scene |
| `s` | Save current state as a preset (empty path = autosave) |
| `n` | Assign newly detected apps using `config/app-rules.json` |

## Source-row keys (input/app rows inside an expanded cable)

| Key | Action |
|---|---|
| `N` / `E` | Toggle RNNoise suppression / "voice" EQ preset on the selected input |
| `H` | Toggle HRTF 3D spatialization on the selected input |
| `Y` | Aim spatialized input: type azimuth (`0`=front, `+90`=left, `-90`=right) and elevation |

## Output keys

| Key | Action |
|---|---|
| `o` | Add a new output from the render-endpoint picker |

## Per-app routing

When a group has a `cable` set, adding an application with `a` routes that
application's Windows output to the cable (via `winappaudiorouter`) and the
mixer captures the cable. This avoids double audio. Without a cable, the
application is captured via process loopback and will still be heard on its
original device.

The TUI warns when an app's live audio session is still on a different device
(`! still on <device> - restart app to apply`). This happens because Windows
per-app routes only take effect when the app recreates its audio stream.
