# AnniAudio — Roadmap

Phases are defined by completion criteria, not dates. Work on phases can overlap where dependencies allow. Phase 0 is the most important — skipping it causes pain later.

---

## Phase 0 — Research and Foundations

**Goal:** Every major component has a working proof of concept. No unknowns going into Phase 1.

- [x] Finalize the config JSON schema — done, see ARCHITECTURE.md Layer 6
- [x] Finalize the REST API schema — done, see ARCHITECTURE.md Layer 4
- [x] Decide Windows version target — **Win11 only**
- [x] Decide UI stack — **Electron**
- [x] Decide NVIDIA model distribution — **download on first run**
- [ ] Read through Microsoft `sysvad` sample driver and understand the structure
- [ ] Write a minimal WDM virtual audio device that appears in Windows (even if it outputs silence)
- [ ] Get WASAPI loopback capture working — capture what's playing on a device
- [ ] Integrate NVIDIA RTX Effects SDK in a test harness — confirm it runs on the GPU
- [ ] Integrate RNNoise in a test harness — confirm it reduces noise on a test signal
- [ ] Load a SOFA HRTF file with libmysofa, apply convolution to a test signal, verify it sounds spatial through headphones
- [ ] Implement a basic biquad parametric EQ and verify it shapes frequency response correctly

**Exit criteria:** Every component listed above works in isolation. No major technical unknowns remain.

---

## Phase 1 — Virtual Driver and Basic Routing

**Target:** Windows 11 only.

**Goal:** AnniAudio appears as an audio device in Windows and audio flows through it end-to-end.

- [ ] Virtual WDM driver built from sysvad, stripped to essentials
- [ ] Driver installer — handles elevation, test signing, `.inf` + `.sys` packaging
- [ ] WASAPI engine — capture from virtual device, render to real output
- [ ] In-memory routing matrix — hardcoded for testing at first
- [ ] Config file loading and saving
- [ ] Verify: set AnniAudio as output device in any app, hear audio through speakers

**Exit criteria:** You can select AnniAudio Cable 1 as your output device in Windows and audio comes out your headphones.

---

## Phase 2 — DSP Chain

**Goal:** Audio flowing through AnniAudio can be processed.

- [ ] DSP chain architecture — ordered list of processing nodes per route
- [ ] Parametric EQ node — full biquad implementation, all filter types
- [ ] Noise cancellation node — NVIDIA primary, RNNoise fallback, both behind `INoiseCanceller`
- [ ] Pre-gain and post-gain nodes
- [ ] DSP chain configurable via config file
- [ ] Audio thread is allocation-free and running at `THREAD_PRIORITY_TIME_CRITICAL`

**Exit criteria:** Mic input through AnniAudio has audible noise removed. EQ visibly shapes the frequency response. CPU usage is reasonable.

---

## Phase 3 — Spatial Audio

**Goal:** HRTF-based spatial audio works and sounds noticeably good through headphones.

- [ ] SOFA file loader via libmysofa
- [ ] FFT-based overlap-add convolution engine using KissFFT
- [ ] HRTF profile selection and switching at runtime
- [ ] MIT KEMAR and at least one other dataset bundled
- [ ] SOFA file loading from user-provided path
- [ ] Spatial audio node plugs into DSP chain

**Exit criteria:** A mono source processed through the spatial node sounds clearly 3D through headphones. Noticeably better than Windows Sonic.

---

## Phase 4 — API, Hotkeys, and CLI

**Goal:** Everything is controllable programmatically, without touching a config file or UI.

- [ ] HTTP REST API server via cpp-httplib
- [ ] All core operations exposed as endpoints (see ARCHITECTURE.md)
- [ ] WebSocket event stream — level meters, device state changes
- [ ] API key auth, loopback-only by default, LAN as explicit opt-in
- [ ] Global hotkeys via Win32 `RegisterHotKey`
- [ ] All hotkey bindings configurable in config JSON
- [ ] CLI client (`anniaudio-cli`) wrapping the API
- [ ] CLI: list devices, get/set routes, load preset, toggle features, adjust gain

**Exit criteria:** Every feature can be triggered from a PowerShell one-liner via the CLI.

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
- Per-app routing — detect which app is sending audio and apply different chains
- VST plugin hosting — use third-party DSP plugins in the chain
- HRTF interpolation — smooth transitions between positions
- Room simulation and reverb node
- AnniWebsite integration — control AnniAudio from the web dashboard
- Non-NVIDIA noise cancellation improvement — better CPU-side model or Whisper-based approach
- Preset cloud sync