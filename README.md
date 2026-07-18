# AnniAudio

One install. One config. Full control over your audio.

A system-wide audio processing suite for Windows 11. Ships its own virtual WDM audio driver, a full parametric EQ, GPU-accelerated noise cancellation, and HRTF-based spatial audio — all in a single background process with a REST API, configurable hotkeys, and a routing matrix that lets you wire audio wherever you want.

Built because SteelSeries Sonar gets close but breaks constantly, has no API, and still isn't flexible enough. And because every other good audio tool is either fragmented across three apps or locks you in with no way out.

Part of the [Anni Ecosystem](https://github.com/KoiNoYume7).

---

## Status

**Phase 1 — Active development**

What works today:

- Virtual WDM driver builds and links (`AnniAudioCable.sys`).
- WASAPI routing engine (`AudioEngine`) compiles and can enumerate endpoints.
- Phase 0 POCs all pass:
  - `poc_eq` — biquad parametric EQ frequency response verified.
  - `poc_rnnoise` — CPU noise cancellation, ~57 dB reduction on white noise.
  - `poc_wasapi` — loopback capture + render, 1 kHz sine round-trip verified.
- `route_cli list` correctly shows all render/capture devices.
- CMake build configured; C++20 core + RNNoise submodule build cleanly.

Not yet finished:

- The virtual driver is **built but not signed**. It cannot load on a normal Windows install without test signing or Microsoft attestation signing.
- `AudioEngine` does not yet expose a WASAPI **loopback-capture** path from a render endpoint (the `poc_wasapi` proves it is possible, but it isn't wired into the reusable engine).
- DSP callbacks (EQ, RNNoise, HRTF) are implemented as standalone POCs but are **not yet connected** to the live audio engine.
- The GUI/TUI are thin prototypes around the CLI, not a polished product UI.

---

## Requirements

- Windows 11
- Visual Studio 2022 BuildTools (or Community/Professional/Enterprise) with the **Desktop development with C++** workload
- CMake **3.20+**
- Windows SDK (tested with 10.0.26100.0)
- Windows Driver Kit (WDK) — only if you want to build the kernel driver
- Git, with submodules support

---

## Clone and build

```powershell
git clone -b dev https://github.com/KoiNoYume7/AnniAudio
cd AnniAudio
git submodule update --init --recursive

# The RNNoise model data is not in git; download and extract it once:
cd third_party/rnnoise
curl -L -O https://media.xiph.org/rnnoise/models/rnnoise_data-0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37.tar.gz
tar -xzf rnnoise_data-0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37.tar.gz
cd ../..
```

### Build the user-mode core and POCs

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Built binaries will be in `build/bin/Release/`.

### Run the POCs

```powershell
# EQ frequency response
.\build\bin\Release\poc_eq.exe

# RNNoise noise cancellation
.\build\bin\Release\poc_rnnoise.exe

# WASAPI loopback: renders a 1 kHz sine and verifies it comes back through loopback capture
.\build\bin\Release\poc_wasapi.exe

# List all audio endpoints
.\build\bin\Release\route_cli.exe list
```

### Build the virtual WDM driver

The driver is a separate MSBuild project, not part of the CMake tree.

```powershell
# 1. Create a cable config from the example
copy config\cables.json.example config\cables.json

# 2. Generate the INF from the template
.\scripts\generate-inf.ps1

# 3. Build the driver with MSBuild
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe' `
    driver\AnniAudioCable.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

If the build succeeds you will have:

```
driver\build\driver\release\AnniAudioCable.sys
driver\build\driver\release\AnniAudioCable.inf
```

---

## Driver signing (the blocker for daily use)

Windows 11 will not load a new kernel-mode driver unless it is **Microsoft-signed**.

### For development (free, but disables some anti-cheat compatibility)

Enable test signing and use a self-signed certificate:

```powershell
.\cli\anniaudio.ps1 dev-mode
# reboot when prompted
.\cli\anniaudio.ps1 install
```

This is fine for testing, but many games with anti-cheat will refuse to run while test signing is enabled. Use `gaming-mode` to disable it again:

```powershell
.\cli\anniaudio.ps1 gaming-mode
# reboot when prompted
```

### For distribution or daily gaming use

You must submit the driver to the Microsoft Hardware Dev Center for **attestation signing**:

1. Purchase an **EV code signing certificate** (the cheapest are around $220–$280/year; DigiCert/GlobalSign are more expensive).
2. Register a Windows Hardware Dev Center account with that EV cert.
3. Submit `AnniAudioCable.sys` + `AnniAudioCable.inf` as a signed `.cab`.
4. Microsoft returns a signed driver package that loads on any Windows 11 machine, even with Secure Boot and anti-cheat enabled.

This is a business expense, not a hobby expense. The recommended path is to finish the product, prove users want it, and then pay for the cert from revenue.

---

## Daily use without the driver (the current realistic path)

You do **not** need the virtual driver to use AnniAudio as a system-wide audio processor. The engine can capture the default output via WASAPI loopback, run it through EQ / RNNoise / HRTF, and render it to your headphones.

This is being wired up now. Once it lands, the workflow will be:

```powershell
.\build\bin\Release\route_cli.exe process "Speakers" "Headphones" --eq presets/headphones.json
```

That command will sit between Windows and your headphones, applying EQ/noise processing in real time, with no driver signing required and no impact on games.

See `docs/ROADMAP.md` for the full breakdown.

---

## Architecture

```
+------------------------------------------------+
|                   AnniAudio                    |
|                                                |
|   +----------+      +----------------------+   |
|   | UI / CLI |      |   REST + WebSocket   |   |
|   +----+-----+      +-----------+----------+   |
|        +--------------------+   |              |
|                        +----v---v----+         |
|                        | Core Engine |         |
|                        +------+------+         |
|            +-----------+------+------+-----+   |
|         +--v--+    +---v----+    +----v--+ |   |
|         | DSP |    | Spatial|    | Noise | |   |
|         | EQ  |    |  HRTF  |    | Cancel| |   |
|         +-----+    +--------+    +-------+ |   |
|                         |                  |   |
|              +----------v-----------+      |   |
|              |    Routing Matrix    |      |   |
|              +----------+-----------+      |   |
|                         |                  |   |
|              +----------v-----------+      |   |
|              | Virtual WDM Driver   |      |   |
|              +----------+-----------+      |   |
+--------------------------------------------|---+
                           |
                  Windows Audio Stack
                   (WASAPI / WDM)
```

Key components:

- `src/core/AudioEngine.cpp` — WASAPI capture/render with a ring buffer and format conversion.
- `src/core/route_cli.cpp` — command-line routing tool built on `AudioEngine`.
- `driver/` — WDM PortCls virtual audio driver.
- `src/dsp/poc_eq.cpp` — biquad parametric EQ POC (all 7 filter types).
- `src/noise/poc_rnnoise.cpp` — RNNoise integration POC.
- `cli/anniaudio.ps1` — control panel for driver install, config, and test-signing mode.

Full technical breakdown in `docs/ARCHITECTURE.md`.

---

## Project Structure

```
AnniAudio/
├── src/
│   ├── core/        # Engine, session management, route_cli
│   ├── driver/      # (placeholder; real driver lives in /driver)
│   ├── dsp/         # EQ and filter POC
│   ├── noise/       # RNNoise and NVIDIA RTX SDK wrappers
│   ├── spatial/     # HRTF / convolution (placeholder)
│   ├── routing/     # Routing matrix logic (placeholder)
│   └── api/         # REST/WebSocket server (placeholder)
├── driver/          # WDM PortCls driver source + .inf template
├── include/         # Public headers
├── cli/             # PowerShell control panel
├── scripts/         # Build helpers, driver scripts, TUI/GUI wrappers
├── config/          # Cable definitions and user settings
├── docs/            # Architecture, roadmap, research
├── third_party/     # Vendored dependencies (rnnoise, etc.)
├── assets/          # Icons, bundled HRTF datasets
├── CMakeLists.txt
└── README.md
```

---

## Roadmap Summary

| Phase | Goal | Status |
|---|---|---|
| 0 | Research — proof of concept for every major component | **Done** (EQ, RNNoise, WASAPI loopback, driver compile) |
| 1 | Virtual driver + WASAPI routing — audio flows through AnniAudio | Engine builds; driver builds but **unsigned** |
| 2 | DSP chain — EQ and noise cancellation working end-to-end | **In progress** (POCs pass, not wired into engine) |
| 3 | Spatial audio — HRTF convolution | Planned |
| 4 | API + hotkeys + CLI | Planned |
| 5 | UI | Planned |
| 6 | Installer and packaging | Planned |

Full detail in `docs/ROADMAP.md`.

---

## Quick Start (once loopback processing lands)

```powershell
# 1. Build
.\cli\anniaudio.ps1 build

# 2. Process the default output through EQ
.\build\bin\Release\route_cli.exe process "Speakers" "Headphones" --eq config/presets/default.json

# 3. Adjust volume while it runs
#   + or =  louder
#   -      quieter
#   v 75   set to 75%
#   q      stop
```

---

## License

MIT — open source, use it freely.

If you use this commercially, consider contributing back.

---

*Built late at night with energy drinks and genuine frustration at the state of Windows audio tooling.*
