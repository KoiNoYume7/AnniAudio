# AnniAudio — Configuration Files

All user-facing configuration lives under `config/`. Files that contain personal
audio layouts or autosaved state are tracked as examples only (`.gitignore`
excludes the live copies).

## Mixer configs (`config/mixers/`)

| File | Purpose |
|---|---|
| `config/mixers/default.json` | Example 2-group mixer layout |
| `config/mixers/loupedeck.json` | 6-group layout matching the Loupedeck plugin actions |
| `config/mixers/main.json` | **Live autosaved mixer state** (gitignored; created at first run) |
| `config/mixers/test_matrix.json` | Test matrix used by manual verification |

A mixer JSON file has these top-level fields:

- `controlPort` (uint16, optional) — TCP port for the local control API. Default `8850`.
- `bindAddress` (string, optional) — IP address the control API binds to. Default `127.0.0.1` (loopback only). Set to `0.0.0.0` to listen on all interfaces (LAN). When binding to anything other than loopback, always set `apiKey`.
- `apiKey` (string, optional) — secret key. If non-empty, every API request (HTTP, SSE, and future WebSocket) must include an `X-API-Key` header matching this value. Empty means no authentication.
- `running` (bool, runtime) — whether the matrix was running when the snapshot was taken.
- `inputs` (array) — input sources.
- `groups` (array) — mix buses / virtual cables.
- `outputs` (array) — render endpoints.

### `inputs` schema

Each element is an `InputConfig`:

```json
{
  "id": 1,
  "name": "Microphone",
  "type": "device",
  "source": "Microphone (Blue Yeti)",
  "denoise": false,
  "eqPreset": "",
  "spatial": false,
  "azimuth": 0.0,
  "elevation": 0.0,
  "hrtfPath": "",
  "peak": 0.0,
  "rms": 0.0
}
```

- `id` (uint32) — stable input identifier. Auto-assigned if omitted on load.
- `name` (string) — display name.
- `type` (string) — `"device"` (WASAPI endpoint or loopback) or `"application"` (process-ID string).
- `source` (string) — endpoint name or decimal process ID.
- `denoise` (bool, default `false`) — RNNoise suppression (only effective at 48 kHz capture).
- `eqPreset` (string, default `""`) — `"voice"` enables the built-in voice EQ; empty is off.
- `spatial` (bool, default `false`) — HRTF binaural spatialization.
- `azimuth` (float, default `0.0`) — 0 = front, +90 = left, -90 = right.
- `elevation` (float, default `0.0`) — 0 = ear level, +90 = above.
- `hrtfPath` (string, default `""`) — path to a SOFA HRTF dataset. Empty means the bundled MIT KEMAR dataset (`assets/hrtf/mit_kemar.sofa`). Changing this restarts the input.
- `peak` / `rms` (float, runtime) — post-fader levels written by the audio thread.

### `groups` schema

Each element is a `GroupConfig`:

```json
{
  "id": 1,
  "name": "Music",
  "color": "#3b82f6",
  "cable": "Music (Virtual Audio Cable)",
  "inputIds": [1, 2],
  "outputIds": ["Speakers (Realtek(R) Audio)"],
  "outputGains": { "Speakers (Realtek(R) Audio)": 100.0 },
  "volume": 100.0,
  "muted": false,
  "knobIndex": null,
  "peak": 0.0,
  "rms": 0.0
}
```

- `id` (uint32) — stable group identifier. Auto-assigned if omitted on load.
- `name` (string) — display name.
- `color` (string, default `"#3b82f6"`) — UI colour.
- `cable` (string, default `""`) — optional render VAC for per-app routing.
- `inputIds` (array of uint32) — inputs summed into this group.
- `outputIds` (array of string) — render outputs this group feeds.
- `outputGains` (object, default `{}`) — output-name → send-gain percent. `100` = unity. Missing outputs default to `100`.
- `volume` (float, default `100.0`) — group fader percent.
- `muted` (bool, default `false`).
- `knobIndex` (int or `null`, optional) — optional physical controller binding (e.g. Loupedeck dial).
- `peak` / `rms` (float, runtime) — group post-fader levels.

### `outputs` schema

Each element is an `OutputSnapshot`:

```json
{
  "name": "Speakers (Realtek(R) Audio)",
  "master": 100.0,
  "muted": false,
  "groupIds": [1, 2],
  "masterPeak": 0.0,
  "masterRms": 0.0
}
```

- `name` (string) — endpoint name; also serves as the stable identifier.
- `master` (float, default `100.0`) — output master volume percent.
- `muted` (bool, default `false`).
- `groupIds` (array of uint32, runtime) — groups currently feeding this output.
- `masterPeak` / `masterRms` (float, runtime) — output post-master levels.

---

## Scenes (`config/scenes/`)

Named level overlays saved at runtime. Each scene stores group volumes/mutes,
send gains, and output masters/mutes. Applied via the TUI `S` key or
`POST /api/scenes/apply`.

`config/scenes/` itself is gitignored; examples may be added later.

---

## App rules (`config/app-rules.json`)

Semi-automatic application assignment rules used by the TUI.

```json
{
  "rules": [
    { "match": "Spotify.exe", "group": "Music" }
  ],
  "ignore": [ "Chrome.exe" ]
}
```

- `rules`: `match` supports `*` and `?` wildcards against the executable name;
  matching apps are auto-routed to the named group's `cable` when they appear on
  the default render device.
- `ignore`: pids matching these names are excluded from new-app detection.

---

## EQ presets (`config/presets/`)

JSON lists of biquad bands:

```json
[
  { "type": "highpass", "freq": 120, "gain": 0, "q": 0.707 },
  { "type": "peak",     "freq": 3000, "gain": 3, "q": 1.0 }
]
```

Supported types: `peak`, `lowpass`, `highpass`, `lowshelf`, `highshelf`, `notch`, `allpass`.

---

## Profiles

The legacy `config/profiles/*.json` format and the `route_cli process` command were
removed. Use a mixer config (`config/mixers/*.json`) to bundle source, output,
volume, preset, and RNNoise on a per-input basis; see the `inputs` field above.

---

## Virtual driver (`config/cables.json`)

Defines the virtual cables the driver creates. Copy from `config/cables.json.example`.

```json
{
  "schema_version": 1,
  "cables": [
    {
      "id": 1,
      "name": "AnniAudio Cable 1",
      "enabled": true,
      "hw_id": "ROOT\\AnniAudioCable",
      "endpoint_name": "AnniAudio Cable 1"
    }
  ],
  "settings": {
    "default_sample_rate": 48000,
    "default_bit_depth": 32,
    "default_channels": 2,
    "allow_96k": true
  }
}
```

`scripts/generate-inf.ps1` parses only `id`, `name`, `enabled`, `hw_id`, and
`endpoint_name` (defaulting `endpoint_name` to `name`). The `settings` object is
present in the example but is not currently used by the generator.

---

## Global hotkeys (`config/hotkeys.json`)

`route_cli mixer` loads `config/hotkeys.json` (sibling to `config/mixers/`).
The file is optional; if it is missing, no hotkeys are registered. Copy
`config/hotkeys.example.json` to `config/hotkeys.json` and edit the bindings.

```json
{
  "hotkeys": [
    { "keys": "Ctrl+Alt+M",    "action": "toggle_group_mute",    "group": "Music" },
    { "keys": "Ctrl+Alt+Plus",  "action": "nudge_group_volume",   "group": "Music", "delta": 5 },
    { "keys": "Ctrl+Alt+Minus", "action": "nudge_group_volume",   "group": "Music", "delta": -5 },
    { "keys": "Ctrl+Shift+M",  "action": "toggle_output_mute",   "output": "Speakers (Realtek(R) Audio)" },
    { "keys": "Ctrl+Shift+Plus",  "action": "nudge_output_volume",  "output": "Speakers", "delta": 5 },
    { "keys": "Ctrl+Shift+Minus", "action": "nudge_output_volume",  "output": "Speakers", "delta": -5 },
    { "keys": "Ctrl+Alt+Left",  "action": "nudge_input_azimuth",  "input": "Microphone", "delta": -10 },
    { "keys": "Ctrl+Alt+Right", "action": "nudge_input_azimuth",  "input": "Microphone", "delta": 10 }
  ]
}
```

Supported actions:

| Action | Fields | Description |
|---|---|---|
| `toggle_group_mute` | `group` (name) | Toggle mute on the named group. |
| `nudge_group_volume` | `group`, `delta` | Adjust group volume by `delta` percent (clamped 0–200). |
| `toggle_output_mute` | `output` (name) | Toggle mute on the named output. |
| `nudge_output_volume` | `output`, `delta` | Adjust output master by `delta` percent (clamped 0–200). |
| `nudge_input_azimuth` | `input` (name), `delta` | Adjust the named input's HRTF azimuth by `delta` degrees. |
| `set_input_direction` | `input`, `delta` | Set the named input's azimuth to `delta` degrees. |

Key names: `a`–`z`, `0`–`9`, `f1`–`f24`, `space`, `tab`, `enter`, `esc`,
`backspace`, `delete`, `insert`, `home`, `end`, `pageup`, `pagedown`, `up`,
`down`, `left`, `right`, `plus` (main keyboard), `minus` (main keyboard),
`add` / `subtract` (numpad), `volume_mute`, `volume_up`, `volume_down`,
`media_next`, `media_prev`, `media_stop`, `media_play_pause`.

Modifiers: `ctrl`, `alt`, `shift`, `win`. Combine with `+`.

---

## TUI sidecar (`scripts/.routed_apps_<port>.json`)

Generated at runtime by the TUI to remember which applications have been routed to
which cables. Gitignored.
