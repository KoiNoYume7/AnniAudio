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

## TUI sidecar (`scripts/.routed_apps_<port>.json`)

Generated at runtime by the TUI to remember which applications have been routed to
which cables. Gitignored.
