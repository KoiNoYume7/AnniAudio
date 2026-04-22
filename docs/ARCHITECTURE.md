# AnniAudio — Architecture

Technical reference for how AnniAudio is structured, what each layer does, and why specific decisions were made.

---

## Design Principles

**One process.** Everything runs in a single background service. No inter-process complexity for the core audio chain.

**Driver-first.** We own a virtual WDM device. Apps see it as real hardware. No dependency on third-party virtual cable software.

**Non-destructive.** If AnniAudio crashes or is killed, Windows audio falls back gracefully. We never break the system.

**API-first.** Every feature accessible through the UI is also accessible through the REST API. The UI is just a client.

**Config as source of truth.** All state lives in a single JSON config file. Restart the service and everything restores exactly.

---

## Layer 0 — Virtual WDM Driver

**What it does:** Creates virtual audio devices that appear in Windows as real hardware. Apps route audio to them as they would any speaker or microphone.

**Technology:** Windows Driver Model (WDM) / PortCls. Written in C, compiled with the Windows Driver Kit (WDK).

**Starting point:** Microsoft's `sysvad` sample driver — a fully functional virtual audio device available at https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad. We build on top of it rather than from scratch. License is MIT.

**What we expose:** N configurable virtual audio devices, each with:
- Configurable sample rate (44100 / 48000 / 96000 Hz)
- Configurable bit depth (16-bit int / 24-bit int / 32-bit float)
- Configurable channel count (mono, stereo, surround)

**Key challenges:**
- Kernel-mode code — bugs cause BSODs, not crashes. Be careful.
- Requires code signing. For development, use test signing mode. For distribution, an EV certificate or Microsoft attestation signing is required.
- Driver must be packaged as a `.inf` + `.sys` bundle with an installer that handles elevation.

**Development signing:**
```powershell
# Enable test signing mode
bcdedit /set testsigning on
# Create a self-signed cert
New-SelfSignedCertificate -Type CodeSigningCert -Subject "AnniAudio Dev" -CertStoreLocation Cert:\CurrentUser\My
```

---

## Layer 1 — WASAPI Audio Engine

**What it does:** Captures audio from apps routing to our virtual devices, feeds it into the processing chain, and renders the result to a real output device.

**Technology:** Windows Audio Session API (WASAPI).

**Key concepts:**
- Capture side: our virtual device receives audio from apps → WASAPI captures it into our buffer
- Render side: after processing, we push audio to the real output device via WASAPI render client
- Loopback: WASAPI supports capturing whatever is playing on an output device via `AUDCLNT_STREAMFLAGS_LOOPBACK`

**Shared vs exclusive mode:**
- Shared mode is the safe default. ~20–100ms latency. Multiple apps share the device.
- Exclusive mode gives ~1–10ms latency but locks the device. Other apps cannot use it simultaneously.
- Initial target: shared mode for capturing, exclusive mode optional for main output.

**Sample rate handling:** WASAPI devices have a mix format (typically 48kHz / 32-bit float). If our virtual device uses a different format, we resample. Use Windows MFT resampler or `libsamplerate`.

---

## Layer 2 — Routing Matrix

**What it does:** Decides where audio goes. Source → Destination, with gain control per route.

**Sources:** virtual cable outputs, physical microphone inputs, loopback captures
**Destinations:** physical output devices, other virtual cables

Any source can route to any destination. Multiple sources mix into one destination. One source fans out to multiple destinations.

**Implementation:** Simple float gain matrix. At each audio frame, for each destination, sum all connected sources multiplied by their gain values. No allocations in the audio loop.

---

## Layer 3 — DSP Chain

Every route passes through a configurable DSP chain. The chain is ordered and each node is individually enabled or disabled.

```
Audio Frame
    |
    v
[Pre-gain]
    |
    v
[Noise Cancellation]
    |
    v
[Parametric EQ]
    |
    v
[Spatial Audio / HRTF]
    |
    v
[Post-gain]
    |
    v
Output
```

### DSP Node: Parametric EQ

Implemented as a series of biquad IIR filters in series.

Filter types: peaking EQ, low shelf, high shelf, low-pass, high-pass, notch, allpass.

Each band stores: frequency (Hz), gain (dB), Q factor, type.

Reference: Audio EQ Cookbook by Robert Bristow-Johnson — https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html

```cpp
struct BiquadCoeffs { double b0, b1, b2, a1, a2; };
struct BiquadState  { double x1, x2, y1, y2; };

double processSample(double x, BiquadCoeffs& c, BiquadState& s) {
    double y = c.b0*x + c.b1*s.x1 + c.b2*s.x2
                      - c.a1*s.y1  - c.a2*s.y2;
    s.x2 = s.x1; s.x1 = x;
    s.y2 = s.y1; s.y1 = y;
    return y;
}
```

Compute coefficients in double precision even if processing in float to avoid precision loss at low frequencies.

### DSP Node: Noise Cancellation

**Primary — NVIDIA RTX Effects SDK**
- Same AI model as RTX Voice, exposed as a developer API
- Requires NVIDIA GPU (GTX 10xx+ with driver tweak, RTX natively)
- SDK: https://developer.nvidia.com/rtx/broadcast/audio-effects/sdk
- Input: 48kHz, mono, float32
- Processing: `NvAFX_Run(handle, &input, &output, numSamples, numChannels)`
- Model files ship with the SDK (~50MB)

For GTX cards that are not officially supported, add this registry value:
```
HKLM\SYSTEM\CurrentControlSet\Services\nvlddmkm\Global\NvAFX
DWORD: MinGPUArch = 0
```

**Fallback — RNNoise**
- Open source, BSD licensed: https://github.com/xiph/rnnoise
- CPU-based recurrent neural network
- Fixed 480-sample frames (10ms at 48kHz), mono only
- ~1-2% CPU on modern hardware
- Works best for speech — less effective for music

Both backends implement the same `INoiseCanceller` interface and are swappable at runtime.

### DSP Node: Spatial Audio (HRTF)

Head-Related Transfer Function processing: simulates how sound reaches each ear from a 3D position, optimized for headphone playback.

**Approach:** FFT-based overlap-add convolution, once per ear per frame.

```
For each audio frame:
  1. FFT(input frame)
  2. Multiply by FFT(left HRTF impulse response)  -> left channel
  3. Multiply by FFT(right HRTF impulse response) -> right channel
  4. IFFT both channels
  5. Overlap-add to output buffer
```

**HRTF datasets bundled:**
- MIT KEMAR — default, no restrictions: http://sound.media.mit.edu/resources/KEMAR.html
- SADIE II — high quality, free: https://www.york.ac.uk/sadie-project/database.html
- Any SOFA-format file the user provides

**Libraries:**
- `libmysofa` — reads SOFA format HRTF files, MIT license: https://github.com/hoene/libmysofa
- `KissFFT` — FFT implementation, BSD license: https://github.com/mborgerding/kissfft

Note: FFTW is faster but GPL licensed. If commercial distribution is ever pursued, stay with KissFFT or pffft (BSD).

---

## Layer 4 — API Server

**Technology:** `cpp-httplib` — header-only HTTP + WebSocket server, MIT licensed: https://github.com/yhirose/cpp-httplib

**Auth:** `X-API-Key` header. Key stored in config. Loopback-only by default. LAN exposure is an explicit opt-in.

**All requests require header:** `X-API-Key: <key>`

**All responses are JSON. Errors:** `{ "error": "<message>" }`

---

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

---

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

---

### DSP Chains

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

---

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

---

### Events (WebSocket)

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

## Layer 5 — Hotkey Engine

**Technology:** `RegisterHotKey` Win32 API for system-wide global hotkeys.

All bindings are defined in the config file. No hardcoded defaults. Hotkey conflicts with other software are detected and reported on startup.

**Available actions:** toggle noise cancellation, toggle spatial audio, switch preset, mute/unmute cable, adjust gain, switch active output device.

---

## Layer 6 — Config System

**File:** `%APPDATA%\AnniAudio\config.json`

**Format:** JSON via nlohmann/json — header-only, MIT licensed: https://github.com/nlohmann/json

**Behavior:** Load on start → restore full state. Write on change (debounced 500ms). If config is missing or corrupt, start with safe defaults and write a new file.

**Schema (finalized):**
```json
{
  "version": 1,
  "virtual_devices": [
    {
      "id": "cable_1",
      "name": "AnniAudio Cable 1",
      "sample_rate": 48000,
      "bit_depth": 32,
      "channels": 2
    }
  ],
  "routes": [
    {
      "id": "r1",
      "source": "cable_1",
      "destination": "physical_out_default",
      "gain_db": 0.0,
      "muted": false
    }
  ],
  "dsp_chains": {
    "r1": [
      {
        "type": "pre_gain",
        "enabled": true,
        "gain_db": 0.0
      },
      {
        "type": "noise_cancel",
        "enabled": true,
        "backend": "nvidia",
        "intensity": 0.8
      },
      {
        "type": "eq",
        "enabled": true,
        "bands": [
          { "type": "high_pass", "freq": 80,   "gain_db": 0.0, "q": 0.707 },
          { "type": "peak",      "freq": 1000, "gain_db": 3.0, "q": 1.0   },
          { "type": "high_shelf","freq": 8000, "gain_db": 2.0, "q": 0.707 }
        ]
      },
      {
        "type": "spatial",
        "enabled": false,
        "hrtf_profile": "kemar",
        "hrtf_sofa_path": null
      },
      {
        "type": "post_gain",
        "enabled": true,
        "gain_db": 0.0
      }
    ]
  },
  "presets": {
    "gaming": {
      "dsp_chains": {}
    }
  },
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

**EQ band types:** `peak` `low_shelf` `high_shelf` `low_pass` `high_pass` `notch` `allpass`

**Noise cancel backends:** `nvidia` `rnnoise` — falls back to `rnnoise` automatically if no NVIDIA GPU is found.

**HRTF profiles:** `kemar` `sadie2` or an absolute path in `hrtf_sofa_path` (overrides profile name when set).

---

## Layer 7 — Electron UI

**Technology:** Electron — build cross-platform desktop apps with web technologies, MIT licensed: https://electronjs.org/

**Purpose:** Provide a user-friendly interface for AnniAudio, accessible from the system tray.

**Features:**

- Device and route management
- DSP chain configuration
- Preset management
- Hotkey configuration
- System tray icon with quick actions

---

## Threading Model

| Thread | Responsibility |
|---|---|
| Main thread | Startup, shutdown, config management |
| Audio thread(s) | WASAPI capture/render loops, DSP chain |
| API thread pool | HTTP server, WebSocket connections |
| Hotkey thread | Win32 message loop for hotkey events |
| UI thread | Electron UI event loop |

Audio thread runs at `THREAD_PRIORITY_TIME_CRITICAL`. The entire DSP chain must be allocation-free in the hot path — no `malloc`, no `new`, no exceptions.

---

## Dependency Table

| Library | Purpose | License |
|---|---|---|
| Windows SDK / WASAPI | Audio capture and render | Windows SDK |
| WDK / PortCls | Kernel audio driver | Microsoft |
| NVIDIA RTX Effects SDK | GPU noise cancellation | NVIDIA (models downloaded on first run, not bundled) |
| RNNoise | CPU noise cancellation fallback | BSD |
| libmysofa | HRTF SOFA file loading | MIT |
| KissFFT | FFT for HRTF convolution | BSD |
| cpp-httplib | HTTP + WebSocket API server | MIT |
| nlohmann/json | JSON config | MIT |
| GoogleTest | Testing | BSD |
| Electron | UI shell (Phase 5) | MIT |

---

## Known Issues and Open Questions

- **Driver signing for distribution:** EV cert or Microsoft attestation signing required. For personal use, test signing mode is sufficient. Microsoft attestation signing is the target for open source distribution.
- **NVIDIA SDK EULA:** Model files are downloaded on first run, not bundled. No redistribution concern.
- **HRTF licensing:** MIT KEMAR (no restrictions) and SADIE II (free) are bundled. IRCAM Listen requires attribution — verify exact text before including. CIPIC is free for research use only. Deferred to Phase 3.
- **Exclusive mode conflicts:** If another app grabs a device in exclusive mode, WASAPI capture from it will fail. Surface this as a clear error in the API and log.
- **FFTW GPL:** KissFFT only. pffft as fallback if performance is insufficient.
- **Windows version target:** Win11 only.