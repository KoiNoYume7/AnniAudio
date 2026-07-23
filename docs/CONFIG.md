# AnniAudio — Configuration Files

All user-facing configuration lives under `config/`. Files that contain personal
audio layouts or autosaved state are tracked as examples only (`.gitignore` excludes
the live copies).

## Mixer configs (`config/mixers/`)

| File | Purpose |
|---|---|
| `config/mixers/default.json` | Example 2-group mixer layout |
| `config/mixers/loupedeck.json` | 6-strip layout matching the Loupedeck plugin actions |
| `config/mixers/main.json` | **Live autosaved mixer state** (gitignored; created at first run) |
| `config/mixers/test_matrix.json` | Test matrix used by manual verification |

Mixer JSON has three top-level arrays:

- `inputs` — device/application sources, each with `id`, `name`, `sourceType`, `source`,
  `volume`, `muted`, plus optional `denoise` and `eqPreset`.
- `groups` — virtual cables (mix buses), each with `id`, `name`, `color`, `volume`,
  `muted`, optional `cable` (render VAC for per-app routing), `outputIds`, and
  `outputGains`.
- `outputs` — render endpoints, each with `id`, `name`, `master`, `muted`.

## Scenes (`config/scenes/`)

Named level overlays saved at runtime. Each scene stores volumes, mutes, send gains,
and output masters. Applied via the TUI `S` key or `POST /api/scenes/apply`.

`config/scenes/` itself is gitignored; examples may be added later.

## App rules (`config/app-rules.json`)

Semi-automatic application assignment rules used by the TUI.

```json
{
  "rules": [
    { "exe": "Spotify.exe", "group": "Music" }
  ],
  "ignore": [ "Chrome.exe" ]
}
```

- `rules`: `exe` supports `*` and `?` wildcards; matching apps are auto-routed to the
  named group's `cable` when they appear on the default render device.
- `ignore`: pids matching these names are excluded from new-app detection.

## EQ presets (`config/presets/`)

JSON lists of biquad bands:

```json
[
  { "type": "highpass", "freq": 120, "gain": 0, "q": 0.707 },
  { "type": "peak",     "freq": 3000, "gain": 3, "q": 1.0 }
]
```

Supported types: `peak`, `lowpass`, `highpass`, `lowshelf`, `highshelf`, `notch`, `allpass`.

## Profiles (`config/profiles/`)

A profile bundles source, output, volume, optional preset, and RNNoise toggle for
the `route_cli process` legacy path:

```json
{
  "source": "Microphone",
  "output": "Headphones",
  "volume": 0.8,
  "preset": "config/presets/clean_voice.json",
  "rnnoise": true
}
```

## Virtual driver (`config/cables.json`)

Defines the virtual cables the driver creates. Copy from `config/cables.json.example`.

```json
{
  "cables": [
    { "id": 1, "name": "AnniAudio Cable 1" }
  ]
}
```

Used by `scripts/generate-inf.ps1` to build the driver INF.

## TUI sidecar (`scripts/.routed_apps_<port>.json`)

Generated at runtime by the TUI to remember which applications have been routed to
which cables. Gitignored.
