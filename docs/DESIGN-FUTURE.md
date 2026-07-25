# AnniAudio — Future Design (NOT IMPLEMENTED)

This file contains design ideas and planned future directions. **Nothing here is
implemented unless another doc explicitly says so.** It is kept for historical
context and to preserve the reasoning behind later decisions. The running system
is described in `docs/ARCHITECTURE.md`.

---

## Layer 4 — API Server v1 (future)

A future, broader control API for the whole product (not just the mixer). It may
use `cpp-httplib` — header-only HTTP + WebSocket server, MIT licensed:
https://github.com/yhirose/cpp-httplib

Compared to the current mixer API (`docs/MIXER-CONTROL-API.md`), this v1 surface
covers device/route CRUD, `X-API-Key` auth, optional LAN exposure, and a WebSocket
event stream. It is listed here as a long-term target once more of the product
exists to expose.

**Auth:** `X-API-Key` header. Key stored in config. Loopback-only by default. LAN
exposure is an explicit opt-in.

**All requests require header:** `X-API-Key: <key>`

**All responses are JSON. Errors:** `{ "error": "<message>" }`

### Devices

`GET /api/v1/devices` — list all devices (virtual and physical)
```json
// 200
[
  { "id": "cable_1", "name": "AnniAudio Cable 1", "type": "virtual",
    "sample_rate": 48000, "channels": 2, "bit_depth": 32 },
  { "id": "physical_out_default", "name": "Speakers (Realtek)", "type": "physical_output" },
  { "id": "physical_in_default",  "name": "Microphone (Blue Yeti)", "type": "physical_input" }
]
```

`POST /api/v1/devices` — create a new virtual cable
```json
// Request
{ "name": "AnniAudio Cable 2", "sample_rate": 48000, "channels": 2, "bit_depth": 32 }
// 201
{ "id": "cable_2", "name": "AnniAudio Cable 2", "type": "virtual",
  "sample_rate": 48000, "channels": 2, "bit_depth": 32 }
```

`DELETE /api/v1/devices/:id` — destroy a virtual cable → `204 No Content`

### Routes

`GET /api/v1/routes` — get the full routing matrix
```json
// 200
[
  { "id": "r1", "source": "cable_1", "destination": "physical_out_default",
    "gain_db": 0.0, "muted": false }
]
```

`POST /api/v1/routes` — create or update a route
```json
// Request
{ "source": "cable_1", "destination": "physical_out_default", "gain_db": -3.0 }
// 200 (update) or 201 (create)
{ "id": "r1", "source": "cable_1", "destination": "physical_out_default",
  "gain_db": -3.0, "muted": false }
```

`DELETE /api/v1/routes/:id` → `204 No Content`

`PATCH /api/v1/routes/:id` — partial update (e.g. mute toggle)
```json
// Request
{ "muted": true }
// 200
{ "id": "r1", "source": "cable_1", "destination": "physical_out_default",
  "gain_db": 0.0, "muted": true }
```

### DSP Chains (configurable chain, future)

`GET /api/v1/chain/:routeId` — get DSP chain for a route
```json
// 200 — ordered array of nodes
[
  { "type": "pre_gain",     "enabled": true, "gain_db": 0.0 },
  { "type": "noise_cancel", "enabled": true, "backend": "nvidia", "intensity": 0.8 },
  { "type": "eq",           "enabled": true, "bands": [
      { "type": "peak", "freq": 1000, "gain_db": 3.0, "q": 1.0 }
  ]},
  { "type": "spatial",     "enabled": false, "hrtf_profile": "kemar", "hrtf_sofa_path": null },
  { "type": "post_gain",   "enabled": true, "gain_db": 0.0 }
]
```

`PUT /api/v1/chain/:routeId` — replace the full DSP chain (same body format as GET response) → `200` with updated chain

`PATCH /api/v1/chain/:routeId/:nodeType` — update a single node
```json
// Request — PATCH /api/v1/chain/r1/noise_cancel
{ "enabled": false }
// 200
{ "type": "noise_cancel", "enabled": false, "backend": "nvidia", "intensity": 0.8 }
```

### Presets

`GET /api/v1/presets` — list saved presets
```json
// 200
[
  { "id": "gaming",  "name": "Gaming" },
  { "id": "podcast", "name": "Podcast" }
]
```

`POST /api/v1/presets` — save current config as a named preset
```json
// Request
{ "name": "Gaming" }
// 201
{ "id": "gaming", "name": "Gaming" }
```

`POST /api/v1/presets/:id/load` — load a preset → `204 No Content`

`DELETE /api/v1/presets/:id` → `204 No Content`

### Events (WebSocket, future)

`WS /api/v1/events` — subscribe to real-time events

All messages are JSON with a `type` field:
```json
// Level meter tick (every 50ms per route)
{ "type": "level", "route_id": "r1", "peak_db": -12.3, "rms_db": -18.7 }

// Device list changed
{ "type": "devices_changed" }

// Route state changed
{ "type": "route_changed", "route_id": "r1", "muted": true }

// Preset loaded
{ "type": "preset_loaded", "preset_id": "gaming" }
```

---

## Layer 5 — Hotkey Engine (future)

**Technology:** `RegisterHotKey` Win32 API for system-wide global hotkeys.

All bindings would be defined in the config file. No hardcoded defaults. Hotkey
conflicts with other software would be detected and reported on startup.

**Planned actions:** toggle noise cancellation, toggle spatial audio, switch
preset, mute/unmute cable, adjust gain, switch active output device.

*Not built:* no `RegisterHotKey` code exists in the current source.

---

## Layer 6 — Config System (future centralized schema)

**File:** `%APPDATA%\AnniAudio\config.json`

**Format:** JSON via nlohmann/json.

**Behavior:** Load on start → restore full state. Write on change (debounced).
If config is missing or corrupt, start with safe defaults and write a new file.

**Planned schema:**
```json
{
  "version": 1,
  "virtual_devices": [
    { "id": "cable_1", "name": "AnniAudio Cable 1", "sample_rate": 48000,
      "bit_depth": 32, "channels": 2 }
  ],
  "routes": [
    { "id": "r1", "source": "cable_1", "destination": "physical_out_default",
      "gain_db": 0.0, "muted": false }
  ],
  "dsp_chains": {
    "r1": [
      { "type": "pre_gain", "enabled": true, "gain_db": 0.0 },
      { "type": "noise_cancel", "enabled": true, "backend": "nvidia", "intensity": 0.8 },
      { "type": "eq", "enabled": true, "bands": [
          { "type": "high_pass", "freq": 80, "gain_db": 0.0, "q": 0.707 }
      ]},
      { "type": "spatial", "enabled": false, "hrtf_profile": "kemar" },
      { "type": "post_gain", "enabled": true, "gain_db": 0.0 }
    ]
  },
  "presets": { "gaming": { "dsp_chains": {} } },
  "hotkeys": {
    "toggle_noise_cancel": "Ctrl+Alt+N",
    "toggle_spatial":      "Ctrl+Alt+S",
    "mute_all":            "Ctrl+Alt+M",
    "preset_next":         "",
    "gain_up":             "",
    "gain_down":           ""
  },
  "api": {
    "port": 7890,
    "key":  "changeme",
    "lan":  false
  },
  "service": {
    "start_with_windows": true,
    "log_level": "info"
  }
}
```

*Current reality:* the mixer stores its state in `config/mixers/main.json` and its
DSP is per-input, not a configurable per-route chain. See `docs/CONFIG.md`.

---

## Layer 7 — Electron UI (future)

**Technology:** Electron — build cross-platform desktop apps with web technologies,
MIT licensed: https://electronjs.org/

**Purpose:** Provide a user-friendly interface for AnniAudio, accessible from the
system tray.

**Planned features:**
- Device and route management
- DSP chain configuration
- Preset management
- Hotkey configuration
- System tray icon with quick actions

*Current reality:* the only control surfaces are the curses TUI and the Loupedeck
plugin. See `docs/TUI.md` and `AnniAudioMixerPlugin/README.md`.

---

## Noise Cancellation — NVIDIA RTX Effects SDK (future)

The original Phase 0 design considered NVIDIA RTX Effects SDK as a possible
primary backend, with RNNoise as a fallback.

**NVIDIA RTX Effects SDK**
- Same AI model as RTX Voice, exposed as a developer API
- Requires NVIDIA GPU (GTX 10xx+ with driver tweak, RTX natively)
- SDK: https://developer.nvidia.com/rtx/broadcast/audio-effects/sdk
- Input: 48 kHz, mono, float32
- For GTX cards that are not officially supported, this registry value removes
  the GPU check:
  ```
  HKLM\SYSTEM\CurrentControlSet\Services\nvlddmkm\Global\NvAFX
  DWORD: MinGPUArch = 0
  ```

*Current reality:* RNNoise is the only noise-cancellation backend. No NVIDIA code
is present in the source.

---

## Per-output Spatial Virtualization (future)

A future use of the `dsp::Spatializer` would be to flag an **output** as spatial,
map a virtual 5.1/7.1 layout to fixed directions, and binauralize the entire mix
for headphone playback. The FFT overlap-add convolution engine is shared with the
already-implemented per-input spatializer.

*Current reality:* spatialization is a per-input property only.

---

## Configurable DSP Chain (future)

The current DSP is hardcoded per input: RNNoise flag, EQ preset name, and HRTF
flag. A future design would replace this with an ordered, configurable list of
DSP nodes per route, exposed through the API and UI.

*Current reality:* see `docs/ARCHITECTURE.md` Layer 3 for the actually-implemented
per-input DSP chain.
