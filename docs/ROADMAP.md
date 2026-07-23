# AnniAudio — Roadmap

Phases are defined by completion criteria, not dates. Work on phases can overlap where dependencies allow. Phase 0 is the most important — skipping it causes pain later.

---

## Phase 0 — Research and Foundations

**Goal:** Every major component has a working proof of concept. No unknowns going into Phase 1.

- [x] Finalize the config JSON schema — done, see `docs/CONFIG.md`
- [x] Finalize the mixer REST API schema — done, see `docs/MIXER-CONTROL-API.md`
- [x] Decide Windows version target — **Win11 only**
- [x] Decide UI stack — **curses TUI + Loupedeck C# plugin; graphical UI later (Electron is a candidate)**
- [x] Read through Microsoft `sysvad` sample driver and understand the structure
- [x] Write a minimal WDM virtual audio device that appears in Windows (driver builds; not signed)
- [x] Get WASAPI loopback capture working — capture what's playing on a device
- [x] Integrate RNNoise in a test harness — confirm it reduces noise on a test signal
- [x] Load a SOFA HRTF file with libmysofa, apply convolution to a test signal, verify it sounds spatial through headphones — `poc_hrtf` (Phase 3 Stage A)
- [x] Implement a basic biquad parametric EQ and verify it shapes frequency response correctly

**Exit criteria:** Every component listed above works in isolation. No major technical unknowns remain.

---

## Phase 1 — Virtual Driver and Basic Routing

**Target:** Windows 11 only.

**Goal:** AnniAudio appears as an audio device in Windows and audio flows through it end-to-end.

- [x] Virtual WDM driver built from sysvad, stripped to essentials
- [x] WASAPI engine — capture from virtual device or loopback, render to real output
- [x] In-memory routing matrix
- [x] Config file loading and saving
- [ ] Driver installer + attestation signing so the driver loads without test signing
- [ ] Verify: set AnniAudio as output device in any app, hear audio through speakers

**Exit criteria:** You can select AnniAudio Cable 1 as your output device in Windows and audio comes out your headphones (requires signed driver).

---

## Phase 2 — DSP Chain + Routing Matrix

**Goal:** Audio flowing through AnniAudio can be processed and routed to multiple outputs.

- [x] `AudioMixer` capture/render engine with real-time mixing loop
- [x] `AudioMixerMatrix` orchestrating multiple outputs with per-group send gains
- [x] Parametric EQ node (`EqChain`) — all filter types
- [x] Noise suppression node (`NoiseSuppressor`) using RNNoise
- [x] Per-strip volume/mute, per-output master/mute
- [x] Send gains and scenes (level snapshots)
- [ ] Audio thread at `THREAD_PRIORITY_TIME_CRITICAL` (currently high priority)
- [ ] Output safety limiter (hard dB ceiling)

**Exit criteria:** Mic input through AnniAudio has audible noise removed. EQ visibly shapes the frequency response. CPU usage is reasonable.

---

## Phase 3 — Spatial Audio

**Goal:** HRTF-based spatial audio works, sounds noticeably good through headphones,
and is a first-class node in the live mixer (not just a standalone POC).

Built once as a reusable `Spatializer` DSP class (`src/dsp/spatializer.{hpp,cpp}`),
then wired into the matrix in two phases: per-input positioning first (daily value),
per-output virtualization second (the Windows Sonic replacement). The engine — FFT
overlap-add convolution on KissFFT, HRIRs from libmysofa — is shared by both.

Dependencies vendored in `third_party/`: **KissFFT** (BSD, FFT), **libmysofa** 1.3.3
(SOFA reader), **miniz** (zlib-compatible inflate for libmysofa's gzip'd HDF5 chunks,
avoids a full zlib build). Default dataset: **MIT KEMAR** at `assets/hrtf/mit_kemar.sofa`.

### Stage A — POC + convolution engine — **DONE**
- [x] Vendor KissFFT + libmysofa (+ miniz) and bundle MIT KEMAR
- [x] `Spatializer` DSP class: `loadHrtf()` (libmysofa resamples to the stream rate),
      `setDirection(az, el)`, real-time-safe `process()` (mono in → interleaved stereo).
      Overlap-add with a fixed tail, so any block size ≤ maxBlock convolves correctly —
      drops straight into the mixer's variable frame counts.
- [x] ITD preserved by placing each ear's HRIR at its libmysofa onset delay.
- [x] `poc_hrtf` verification harness (`build/bin/Release/poc_hrtf.exe`):
      - block-size invariance (Δ ≈ 6e-7), impulse response == dataset HRIR (Δ ≈ 3e-8)
      - physical spatial cues: ILD symmetric to ±11.8 dB, ITD to ±667 µs (the human max)
      - renders `hrtf_orbit_48k.wav`, a binaural orbit for the human "does it sound 3D?" gate.

### Stage B — Per-input positioning (mixer integration) — **DONE**
- [x] `spatial` + `azimuth`/`elevation` on the **input** config model
      (mirrors `denoise` / `eqPreset`), through matrix → control server → JSON persistence.
- [x] A spatialized strip keeps its left/right channels and virtualizes them:
      each channel is convolved as its own virtual speaker (left at azimuth+30°,
      right at azimuth-30°) and the two binaural results are summed — this preserves
      the stereo image and externalizes it instead of collapsing to a mono point.
      The engine change lives in `AudioMixer::Impl::writeStripSpatial()` (two
      `Spatializer` instances per strip). All state is pre-allocated in
      `setupStripDsp()` before the RT thread; wider-than-stereo outputs get the
      binaural pair in channels 0/1. (A future mono point-source mode can place a
      single source at an exact azimuth for callouts/voices.)
- [x] Live, glitch-free direction changes: `setStripDirection()` queues an atomic
      request applied on the audio thread between blocks (no HRIR-swap race); the
      overlap tail is kept so a moving source cross-fades instead of clicking.
      `POST /api/inputs/{id}/direction` is the live path; `PATCH` toggles spatial on/off.

### Stage C — Surface + document
- [x] TUI: `H` toggles spatial, `Y` aims it, `3D±az` tag on the source row.
- [x] `docs/MIXER-CONTROL-API.md` documents the fields + the direction endpoint.
- [ ] Loupedeck: an azimuth dial action (a physical knob is the natural fit).
- [ ] Optional daily-phase polish: HRTF dataset selection, reverb for externalization.
- [ ] Flip README Phase 3 to "in daily use" once it's had real mileage.

### Stage D — Per-output virtualization (Windows Sonic replacement)
- [ ] Flag an output as spatial; map a virtual 5.1/7.1 layout to fixed directions and
      binauralize the mix — reuses the same `Spatializer`, near-free once Stage B ships.
- [ ] Bundle SADIE II as the high-quality dataset option; support user-provided SOFA paths.

**Exit criteria:** A mono source positioned via a per-input direction sounds clearly 3D
through headphones and tracks its position live from the TUI/Loupedeck. Noticeably
better than Windows Sonic.

---

## Phase 4 — API, Hotkeys, and CLI

**Goal:** Everything is controllable programmatically, without touching a config file or UI.

- [x] HTTP REST + SSE control API via cpp-httplib (`route_cli mixer`)
- [x] Mixer core operations exposed as endpoints (see `docs/MIXER-CONTROL-API.md`)
- [ ] WebSocket event stream (currently SSE; WebSocket for v1 API later)
- [ ] API key auth, loopback-only by default, LAN as explicit opt-in (current mixer is loopback/no-auth)
- [ ] Global hotkeys via Win32 `RegisterHotKey`
- [ ] All hotkey bindings configurable in config JSON
- [ ] Standalone CLI client (`anniaudio-cli`) wrapping the API
- [ ] CLI: list devices, get/set routes, load preset, toggle features, adjust gain

**Exit criteria:** Every feature can be triggered from a PowerShell one-liner via a dedicated CLI.

---

## Phase 5 — UI

**Stack: Electron.** Talks to the same REST API and WebSocket as the CLI — no special IPC needed.

**Goal:** A control panel that's actually good to use.

- [ ] Routing matrix view with visual drag-and-drop wiring
- [ ] Per-cable mixer — gain sliders, mute buttons, live level meters
- [ ] EQ editor — visual frequency response curve, draggable bands
- [ ] Noise cancellation toggle and threshold slider
- [ ] Spatial audio toggle and HRTF profile selector
- [ ] Preset manager — save, load, rename, delete
- [ ] Hotkey configuration panel
- [ ] System tray icon — quick mute, preset switch, open UI
- [ ] Settings page — API port, startup behavior, device defaults

**Exit criteria:** The UI exposes every feature and is genuinely pleasant to use.

---

## Phase 6 — Installer and Packaging

**Goal:** One installer. One reboot. Done.

- [ ] NSIS or WiX installer
- [ ] Driver signing: test signing instructions for personal use, Microsoft attestation signing for open source distribution
- [ ] NVIDIA model download script — installer fetches models from NVIDIA on first run, caches in `%APPDATA%\AnniAudio\models\`
- [ ] AnniAudio registered as a Windows service — starts with Windows, runs in background
- [ ] Graceful startup and shutdown handling
- [ ] Uninstaller removes driver cleanly and leaves no trace
- [ ] GitHub Actions CI — builds on push, runs tests
- [ ] GitHub releases with installer artifacts attached

**Exit criteria:** Download installer, run it, reboot. AnniAudio is running. Uninstall leaves the system exactly as it was.

---

## Backlog

These are not in scope for the initial build but will be added later:

- Compressor and limiter DSP node
- [x] Per-app routing — detect which app is sending audio and route to a group cable
- VST plugin hosting — use third-party DSP plugins in the chain
- HRTF interpolation — smooth transitions between positions
- Room simulation and reverb node
- AnniWebsite integration — control AnniAudio from the web dashboard
- Non-NVIDIA noise cancellation improvement — better CPU-side model or Whisper-based approach
- Preset cloud sync