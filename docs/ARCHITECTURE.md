# AnniAudio — Architecture

This document describes the parts of AnniAudio that exist and run today. For the
long-term design that is not yet implemented, see `docs/DESIGN-FUTURE.md`.

---

## Design Principles

**One process.** Everything runs in a single background service (`route_cli mixer`). No inter-process complexity for the core audio chain.

**Driver-first (eventually).** The goal is to own a virtual WDM device so apps see it as real hardware. The driver in `driver/` builds but is not yet Microsoft-signed, so it cannot load on a normal Windows install. Daily use today uses WASAPI loopback plus a third-party virtual cable (e.g. VB-Cable) for application routing.

**Non-destructive.** If AnniAudio crashes or is killed, Windows audio falls back gracefully. We never break the system.

**API-first.** Every feature exposed by the TUI or plugin is also exposed by the local REST+SSE control API. The UI is just a client.

**Config as source of truth.** All mixer state lives in `config/mixers/main.json` (autosaved while running) and supporting files under `config/`. Restart the mixer and it restores exactly.

---

## Source Layout

| Directory | What it is | Status |
|---|---|---|
| `src/core/` | `audio_core` static library: `AudioMixer`, `AudioMixerMatrix`, `MixerControlServer`, `AudioEngine`, `audio_utils`, `midi_input` | Active |
| `src/dsp/` | `audio_dsp` static library: `EqChain`, `NoiseSuppressor`, `Spatializer` | Active |

| `cli/` | `anniaudio.ps1` PowerShell control panel for driver install/signing/config (`tui` launches `mixer-tui.bat`). Not the Phase 4 API-wrapping CLI | Active |
| `AnniAudioMixerPlugin/` | Loupedeck / Logi Actions C# plugin that talks to the control API | Active |
| `scripts/` | TUI (`mixer_tui.py`), build/driver helpers, PowerShell mode toggles | Active |
| `driver/` | WDM PortCls virtual audio driver source and `.inf` template | Built but unsigned |
| `tests/` | Phase-0 POCs (`poc_*`) and verification tools (`test_routing`, `test_mixer_live_edit`) | Not built by default; see `docs/CONTRIBUTING.md` |
| `config/` | Mixer configs, scenes, app-rules, presets, profiles, cables | Runtime state |

---

## Layer 0 — Virtual WDM Driver

**What it does:** Creates virtual audio devices that appear in Windows as real hardware. Apps route audio to them as they would any speaker or microphone.

**Technology:** Windows Driver Model (WDM) / PortCls. Written in C, compiled with the Windows Driver Kit (WDK).

**Starting point:** Microsoft's `sysvad` sample driver — a fully functional virtual audio device available at https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad. We build on top of it rather than from scratch. License is MIT.

**What we expose:** N configurable virtual audio devices, each with a render and a capture endpoint. The number and names are controlled by `config/cables.json` and emitted into `AnniAudioCable.inf` by `scripts/generate-inf.ps1`.

**Current status:** The driver builds and can be installed in test-signing mode. It is not Microsoft-signed, so the daily mixer path does not use it.

**Key challenges:**
- Kernel-mode code — bugs cause BSODs, not crashes. Be careful.
- Requires code signing. For development, use test signing mode. For distribution, an EV certificate or Microsoft attestation signing is required.
- Driver must be packaged as a `.inf` + `.sys` + `.cat` bundle with an installer that handles elevation.

---

## Layer 1 — WASAPI Audio Engine

**What it does:** Captures audio from physical devices, render-loopback endpoints, or per-process loopback, runs it through capture-side DSP, and renders the result to a real output device.

**Technology:** Windows Audio Session API (WASAPI), running in shared mode.

**Key concepts:**
- Capture side: a physical microphone, a render endpoint via `AUDCLNT_STREAMFLAGS_LOOPBACK`, or a specific process via `ActivateAudioInterfaceAsync` with `PROCESS_LOOPBACK_MODE`.
- Render side: after processing, we push audio to the selected render endpoint via a WASAPI render client.
- Each output is managed by one `AudioMixer` instance (`src/core/AudioMixer.cpp`).

**Format handling:** WASAPI devices expose a mix format (typically 48 kHz / 32-bit float). The mixer opens capture clients in that device's preferred format and resamples to the render rate internally using a per-channel fractional-phase resampler. No external resampler library is used today.

---

## Layer 2 — Routing Matrix

**Implementation:** `AudioMixerMatrix` (`src/core/AudioMixerMatrix.cpp`) orchestrates one `AudioMixer` per physical output.

Audio flows one way: **inputs → groups → outputs**.

- An **input** is a single capture source (device or application). It gets stable `InputId` and carries capture-side DSP flags.
- A **group** is a mix bus (virtual cable). It has one fader/mute, a colour, an optional `cable` for per-app routing, a list of `inputIds`, and a list of `outputIds`.
- An **output** is a render endpoint with a master fader/mute.

For each `(input, output)` pair that a group's `outputIds` demands, `AudioMixerMatrix` opens a strip on that output's `AudioMixer`. Sending a group with one input to two outputs therefore opens the input twice; a future shared group bus may collapse that, but the model, API, and TUI already treat groups as the single entity.

**Cost.** Because each `(input, output)` route is a separate strip, the capture-side DSP described in Layer 3 — RNNoise, EQ, and the two-FFT HRTF spatializer — is instantiated and run once per route, not once per input. A microphone with noise suppression, voice EQ, and HRTF enabled that is sent to three outputs will run three RNNoise instances, three EQ chains, and six HRTF convolutions. CPU cost therefore scales with `inputs × outputs` for routed audio.

**Consistency.** Running DSP per-route instead of per-input is expensive, but it guarantees that every output hears the same processed signal (the same cleaned/EQ'd/spatialized input), and it lets per-output format differences (sample rate, channel count) be handled independently. A shared group bus would move the DSP to a single capture-side pass and mix the processed signal to all outputs; that is the intended future shape.

The effective volume of a strip is `group.volume × outputGains[output]`. The output then applies its own `master` volume/mute. All level changes use atomics and do not glitch audio.

---

## Layer 3 — DSP Chain

DSP is **per-input (per strip)**, not per-route. Processing happens once at capture time, before resampling and before the signal is routed to multiple outputs. This guarantees that a noise-suppressed, EQ'd, or spatialized input sounds the same through every output.

For each strip, `AudioMixer::Impl::setupStripDsp()` allocates:
- `NoiseSuppressor` (RNNoise) if `denoise == true` and capture rate is 48 kHz
- `EqChain` if `eqPreset == "voice"`
- Two `dsp::Spatializer` instances plus a colour-compensation EQ if `spatial == true` and the output has at least two channels

The runtime order in `AudioMixer::Impl::processStrip()` is:

```
captured frame (source rate)
    -> RNNoise denoise (source rate)
    -> EQ (source rate)
    -> [if spatial] split to L/R, resample to render rate,
        convolve each channel as its own virtual speaker, sum,
        apply colour-compensation EQ
    -> [otherwise] resample/convert to render rate
    -> write to ring buffer

render loop (per output):
    -> read each strip's ring buffer
    -> apply strip volume/mute
    -> sum into mix buffer
    -> apply output master volume/mute
    -> clamp and write to render device
```

**Spatializer stereo preservation:** A spatialized source keeps its left/right channels and convolves each as its own virtual speaker (left at `azimuth+spread`, right at `azimuth-spread`). The two binaural results are summed, preserving the stereo image and externalizing it instead of collapsing to a mono point. Live direction changes are applied between blocks; they are glitch-free and safe at knob-turn rates.

### DSP Node: Parametric EQ

Implemented as a series of biquad IIR filters in series (`src/dsp/eq.cpp`).

Filter types: peaking EQ, low shelf, high shelf, low-pass, high-pass, notch, allpass.

Each band stores: frequency (Hz), gain (dB), Q factor, type.

Reference: Audio EQ Cookbook by Robert Bristow-Johnson — https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html

Coefficients are computed in double precision even when audio buffers are float, to avoid precision loss at low frequencies.

### DSP Node: Noise Suppression

**RNNoise** — open source, BSD licensed: https://github.com/xiph/rnnoise
- CPU-based recurrent neural network.
- Fixed 480-sample frames (10 ms at 48 kHz), mono only.
- ~1–2 % CPU on modern hardware.
- Works best for speech; less effective for music.

RNNoise is the only noise-cancellation backend in the current code. It is skipped for sources that are not 48 kHz.

### DSP Node: Spatial Audio (HRTF)

`src/dsp/spatializer.cpp` performs FFT-based overlap-add convolution, once per ear per channel.

**HRTF dataset bundled:**
- MIT KEMAR — default, no restrictions: http://sound.media.mit.edu/resources/KEMAR.html

SOFA files can be loaded via `libmysofa`.

**Libraries:**
- `libmysofa` — reads SOFA format HRTF files, MIT license: https://github.com/hoene/libmysofa
- `KissFFT` — FFT implementation, BSD license: https://github.com/mborgerding/kissfft

---

## Layer 4 — API Server

**Technology:** `cpp-httplib` — header-only HTTP server, MIT licensed: https://github.com/yhirose/cpp-httplib

**Current implementation:** The mixer process (`route_cli mixer`) embeds an HTTP+SSE server bound to `127.0.0.1` on the configured `controlPort` (default **8850**, set by the `--port` CLI flag or a `controlPort` field in the mixer JSON). It is loopback-only and has no authentication. The full endpoint list is in `docs/MIXER-CONTROL-API.md`.

The server runs a thread pool for HTTP handlers and a dedicated broadcast thread for SSE. Every state change broadcasts a full `state` event to connected clients.

---

## Layer 5 — Config System

**Files:**
- `config/mixers/main.json` — live autosaved mixer state (`running`, `controlPort`, `inputs`, `groups`, `outputs`).
- `config/scenes/*.json` — named level overlays (group volumes/mutes/send gains, output masters/mutes).
- `config/app-rules.json` — semi-automatic app assignment rules for the TUI.
- `config/presets/*.json` — biquad EQ band lists.
- `config/profiles/*.json` — bundles for the legacy `route_cli process` path.
- `config/cables.json` — virtual cable list used by `scripts/generate-inf.ps1`.

**Technology:** nlohmann/json — header-only, MIT licensed: https://github.com/nlohmann/json

**Behavior:** `AudioMixerMatrix::load()` reads a mixer preset, `save()` writes it, and `maybeAutosave()` writes `m_autosavePath` after every mutating API call.

---

## Threading Model

| Thread | Responsibility |
|---|---|
| Main / CLI thread | Parses config, creates `AudioMixerMatrix`, calls `start()`, runs the blocking HTTP server |
| Audio thread (one per `AudioMixer` output) | WASAPI capture/render loops, per-strip DSP, mixing |
| HTTP worker pool | cpp-httplib request handlers (call into `AudioMixerMatrix` under its mutex) |
| SSE broadcast thread | Pushes state events to connected clients |

Each `AudioMixer` audio thread calls `SetThreadPriority(..., THREAD_PRIORITY_TIME_CRITICAL)` and `AvSetMmThreadCharacteristics(L"Audio", ...)`. The DSP hot path is allocation-free: all DSP state, ring buffers, and mix buffers are allocated before the thread starts. Structural changes (add/remove/rename strip) are queued and applied at the top of the audio thread's main loop before `WaitForMultipleObjects`, so they never interrupt another strip's render chain.

---

## Dependency Table

| Library | Purpose | License |
|---|---|---|
| Windows SDK / WASAPI | Audio capture and render | Windows SDK |
| WDK / PortCls | Kernel audio driver | Microsoft |
| RNNoise | CPU noise suppression | BSD |
| libmysofa | HRTF SOFA file loading | MIT |
| KissFFT | FFT for HRTF convolution | BSD |
| cpp-httplib | HTTP + SSE API server | MIT |
| nlohmann/json | JSON config | MIT |

---

## Known Issues and Open Questions

- **Driver signing for distribution:** EV cert or Microsoft attestation signing required. For personal use, test signing mode is sufficient. Microsoft attestation signing is the target for open source distribution.
- **HRTF licensing:** MIT KEMAR (no restrictions) is bundled. SADIE II (free) and user-provided SOFA files are supported. IRCAM Listen requires attribution and is not bundled. See `docs/RESEARCH.md` for historical notes.
- **Exclusive mode conflicts:** If another app grabs a device in exclusive mode, WASAPI capture from it will fail. The mixer surfaces this as a failure to add the input.
- **FFTW GPL:** Not used. KissFFT only; pffft is a future fallback if performance is insufficient.
- **Windows version target:** Win11 only.
