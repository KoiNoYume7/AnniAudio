# AnniAudio

One install. One config. Full control over your audio.

A system-wide audio processing suite for Windows 11. It ships its own virtual WDM audio driver, a full parametric EQ, GPU-accelerated noise cancellation, and HRTF-based spatial audio — all in a single background process with a REST API, configurable hotkeys, and a routing matrix that lets you wire audio wherever you want.

The goal is to replace the fragmented stack of Voicemeeter, RTX Voice, VB-Cable, and Windows Sonic with one cohesive, API-first tool.

Part of the [Anni Ecosystem](https://github.com/KoiNoYume7).

---

## Status

**Phase 2 — DSP + routing matrix in daily use**

What works today:

- **Live routing matrix** (`AudioMixerMatrix`): any number of inputs → colour-coded groups (virtual cables) → any number of outputs, edited live over a REST + SSE control API with no audio glitches. State autosaves continuously.
- **Per-app routing**: applications are routed to a group's cable via `winappaudiorouter`, or captured directly via **Windows process loopback** (`ActivateAudioInterfaceAsync`), avoiding double audio.
- **Semi-automatic app assignment**: apps landing on the default device are matched against `config/app-rules.json` and auto-routed; unknown apps queue for one-tap assignment instead of being guessed.
- **Per-output send gains and mute**: one group can play at different levels into different outputs (e.g. full on headphones, quiet on speakers) and each output has its own mute.
- **Mic processing**: RNNoise suppression and a "voice" EQ preset run engine-side per input, so the cleaned signal follows the mic through every route (e.g. into Discord via a Mic cable).
- **Scenes**: named level snapshots (volumes, mutes, send gains, output masters) applied instantly by name.
- **Loupedeck plugin** (`AnniAudioMixerPlugin/`): a real Logi Actions C# SDK plugin with dials and touch buttons for group/output volume, mute, and scene recall, driven entirely through the control API.
- **Autostart at logon**, a curses **TUI** front-end, and Phase 0 POCs (`poc_eq`, `poc_rnnoise`, `poc_wasapi`) all still pass.

Not yet finished:

- The virtual driver is **built but not signed**. It cannot load on a normal Windows install without test signing or Microsoft attestation signing. Daily use currently relies on a third-party virtual cable driver plus WASAPI loopback.
- **HRTF** spatial audio has a working, verified standalone POC (`poc_hrtf`: FFT
  overlap-add convolution over MIT KEMAR HRIRs, with correct ITD/ILD cues) but is
  not yet wired into the mixer — per-input positioning is the next step.
- No graphical mixer UI yet (the TUI and the Loupedeck plugin are the current surfaces); no installer.

---

## Requirements

- Windows 11
- Visual Studio 2022 BuildTools (or Community/Professional/Enterprise) with the **Desktop development with C++** workload
- CMake **3.20+**
- Windows SDK (tested with 10.0.26100.0)
- Windows Driver Kit (WDK) — only needed to build the kernel driver
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

# HRTF spatializer: verifies the convolution engine + spatial cues against MIT KEMAR,
# and renders hrtf_orbit_48k.wav (a source circling your head) for a listening check.
# Run from the repo root so assets/hrtf/mit_kemar.sofa resolves.
.\build\bin\Release\poc_hrtf.exe

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

## Driver signing

Windows 11 will not load a new kernel-mode driver unless it is **Microsoft-signed**.

### For development

Enable test signing and use a self-signed certificate:

```powershell
.\cli\anniaudio.ps1 dev-mode
# reboot when prompted
.\cli\anniaudio.ps1 install
```

This is free and fine for testing, but many games with anti-cheat will refuse to run while test signing is enabled. Use `gaming-mode` to disable it again:

```powershell
.\cli\anniaudio.ps1 gaming-mode
# reboot when prompted
```

### For distribution or daily gaming use

Submit the driver to the Microsoft Hardware Dev Center for **attestation signing**:

1. Purchase an **EV code signing certificate** (cheapest are around $220–$280/year; DigiCert/GlobalSign are more expensive).
2. Register a Windows Hardware Dev Center account with that EV cert.
3. Submit `AnniAudioCable.sys` + `AnniAudioCable.inf` as a signed `.cab`.
4. Microsoft returns a signed driver package that loads on any Windows 11 machine, even with Secure Boot and anti-cheat enabled.

Attestation signing is a business expense, not a hobby expense. The recommended path is to finish the product, prove users want it, and then pay for the cert from revenue.

---

## Daily use without the driver

The virtual driver is not required to use AnniAudio as a system-wide audio processor. The engine can capture any output via WASAPI loopback, run it through EQ / RNNoise / HRTF, and render it to another device.

```powershell
# Passthrough loopback
.\build\bin\Release\route_cli.exe process "Speakers" "Headphones"

# EQ from a JSON preset
.\build\bin\Release\route_cli.exe process "Speakers" "Headphones" 80 --preset config/presets/headphones.json

# RNNoise noise suppression (capture source must be 48 kHz)
.\build\bin\Release\route_cli.exe process "Microphone" "Headphones" 80 --rnnoise

# EQ + RNNoise combined
.\build\bin\Release\route_cli.exe process "Microphone" "Headphones" 80 --preset config/presets/clean_voice.json --rnnoise

# Load everything from a profile
.\build\bin\Release\route_cli.exe process --config config/profiles/voice.json
```

All of these run in real time, with no driver signing required and no impact on games. Preset files live in `config/presets/` and define a list of biquad bands (`peak`, `lowshelf`, `highshelf`, `lowpass`, `highpass`, `notch`, `allpass`). Profiles live in `config/profiles/` and bundle source, output, volume, preset, and RNNoise toggle. RNNoise currently requires a 48 kHz source; it will be skipped otherwise.

### Mixer (MIXLINE replacement)

The `mixer` command mixes any number of WASAPI sources into one output with per-strip volume and mute. It is designed to replace the Logitech MIXLINE-style workflow.

```powershell
# Run the example mixer
.\build\bin\Release\route_cli.exe mixer config/mixers/default.json

# 6-strip layout for Loupedeck Live control (Game / Music / Chat / Ext / System / Mic)
.\build\bin\Release\route_cli.exe mixer config/mixers/loupedeck.json
```

Mixer config files live in `config/mixers/` and describe the full matrix: `inputs` (device or application sources, each with optional `denoise` / `eqPreset`), `groups` (virtual cables with a fader, mute, colour, optional per-app `cable`, `outputIds`, and per-output `outputGains`), and `outputs` (render endpoints with a `master` volume and `muted`). The running mixer autosaves to `config/mixers/main.json`.

The mixer embeds a local HTTP+SSE control API (bound to `127.0.0.1`, default port `8850`) that any client can drive — see `docs/MIXER-CONTROL-API.md` for the full surface. Three clients ship today: the curses **TUI** (below), the **Loupedeck plugin** (`AnniAudioMixerPlugin/`), and any HTTP client. A graphical mixer GUI is still on the roadmap.

#### Mixer TUI

`scripts/mixer_tui.py` is a curses terminal UI for the mixer control API: a live, color-coded view of every virtual cable and output with volume, mute, and routing, all driven from the keyboard while the mixer keeps running in the background. It's a thin REST/SSE client — it never touches audio directly, so it can be closed and reopened at any time without affecting playback, and it stays in sync with any other connected client (GUI, a future Loupedeck plugin, etc.) over the same API.

```powershell
# 1. Start the mixer (opens its control API on port 8850 by default)
.\start-mixer.bat

# 2. In another terminal, start the TUI
.\mixer-tui.bat
```

| Key | Action |
|---|---|
| `Tab` | Switch between the Virtual Cables and Outputs panes |
| `j`/`k`, arrows | Move the selection |
| `+`/`-` | Nudge volume by 5% |
| `v` | Type an exact volume (0-200%) |
| `m` | Toggle mute on the selected virtual cable / output |
| `x` | Set the selected virtual cable's send level towards one of its outputs (e.g. 100% on headphones, 15% on speakers) |
| `Enter`/`e` | Expand/collapse a virtual cable's application list |
| `i` | Add a device source to the selected virtual cable (entries labeled `[cable]`, `[microphone]`, `[system audio (loopback)]`) |
| `a` | Add a running application to the selected virtual cable (uses loopback capture, or per-app routing to the group cable if one is set) |
| `g` | Add a virtual cable |
| `C` | Set the selected virtual cable's group cable (render VAC) for per-app application routing |
| `o` | Add an output |
| `c` | Connect/disconnect the selected virtual cable and output |
| `n` | Assign newly detected apps (semi-automatic routing; rules in `config/app-rules.json`) |
| `N` / `E` | On a source row: toggle RNNoise suppression / the "voice" EQ preset for that input (processed engine-side, pre-mix; shown as `NS` / `EQ` tags) |
| `S` | Scenes: apply a saved level overlay or save the current levels as a new scene (`config/scenes/`) |
| `r` | Rename the selected virtual cable |
| `d` | Delete the selected virtual cable/application/output |
| `s` | Save the current state as a preset (empty path = autosave) |
| `R` | Refresh the endpoint and application lists |
| `q` | Quit (`Esc` only cancels dialogs; pickers support type-to-filter) |

When a group has a `cable` set, adding an application with `a` routes that application's Windows output to the cable (via `AudioPolicyConfig` / `winappaudiorouter`) and the mixer captures the cable. This avoids double audio. Without a cable, the application is captured via process loopback and will still be heard on its original device.

Requires `windows-curses` (`mixer-tui.bat` installs it automatically if missing; otherwise `python -m pip install windows-curses`). Run it directly with `python scripts/mixer_tui.py [--port 8850]` if you'd rather skip the batch wrapper.

#### Autostart at logon

```powershell
.\install-autostart.bat    # start the mixer hidden at every logon (no admin needed)
.\uninstall-autostart.bat  # remove it again
.\stop-mixer.bat           # stop a running mixer (e.g. before rebuilding route_cli)
```

This drops a small `.vbs` into the current user's Startup folder that launches `route_cli.exe mixer` with a hidden window. It deliberately is not a Windows service: WASAPI audio endpoints only exist inside the user session, so the mixer has to start at logon, not at boot. Config is loaded from `config/mixers/main.json` (autosaved continuously while running), so a force-stop never loses state.

#### Loupedeck plugin

`AnniAudioMixerPlugin/` is a Logi Actions C# SDK plugin (`.NET 8`) that controls the mixer from a Loupedeck / Razer Stream Controller device. It talks only to the control API — no direct audio access — so it stays in sync with the TUI and any other client. Build it with `dotnet build AnniAudioMixerPlugin/src/AnniAudioMixerPlugin.csproj -c Release`; a post-build step links it into the Logi Plugin Service and reloads it. Actions:

- **Cable Volume** dial (one per group): turn = volume, press = mute.
- **Output Master** dial (one per output): turn = master volume, press = mute.
- **Mute** touch button (one per group): shows name + level, one-tap toggle.
- **Scene** touch button (one per saved scene): one-tap recall.

The action lists mirror live mixer state, so groups/outputs/scenes appearing, disappearing, or being renamed update the device automatically. If the mixer is not running, the plugin reports an error status.

`route_cli midi list` / `route_cli midi <device-hint>` remain available as a standalone diagnostic to inspect raw MIDI messages from any connected controller; it is not used to control the mixer (the Loupedeck plugin above is the supported control surface).

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

- `src/core/AudioMixer.cpp` — single-output WASAPI mixer: sums N capture strips (device / render-loopback / process-loopback) into one render endpoint, with per-strip volume/mute, master volume/mute, and capture-side DSP.
- `src/core/AudioMixerMatrix.cpp` — the routing matrix over multiple `AudioMixer` outputs: inputs, groups (cables), per-output send gains, preset/scene persistence.
- `src/core/MixerControlServer.cpp` — local HTTP + SSE control API.
- `src/core/AudioEngine.cpp` — original single source→output engine (WASAPI capture/render, ring buffer, format conversion) used by the `process` CLI command.
- `src/dsp/` — `EqChain` (biquad parametric EQ) and `NoiseSuppressor` (RNNoise), built as the `audio_dsp` library and linked into the mixer.
- `src/core/route_cli.cpp` — CLI entry point (`list`, `process`, `mixer`, `midi`).
- `AnniAudioMixerPlugin/` — Loupedeck control plugin.
- `driver/` — WDM PortCls virtual audio driver.
- `cli/anniaudio.ps1` — control panel for driver install, config, and test-signing mode.

Full technical breakdown in `docs/ARCHITECTURE.md`.

---

## Project Structure

```
AnniAudio/
├── src/
│   ├── core/        # Engine, mixer matrix, control server, route_cli
│   ├── dsp/         # EQ chain + RNNoise wrapper (audio_dsp library)
│   ├── noise/       # RNNoise POC
│   └── spatial/     # HRTF / convolution (placeholder)
├── AnniAudioMixerPlugin/  # Loupedeck (Logi Actions C#) plugin
├── driver/          # WDM PortCls virtual audio driver source + .inf template
├── include/         # Public headers
├── cli/             # PowerShell control panel (driver install / signing)
├── scripts/         # mixer_tui.py and build/driver helpers
├── config/          # mixers/ (matrix configs), scenes/, app-rules.json, presets/, profiles/, cables
├── docs/            # ARCHITECTURE, ROADMAP, MIXER-CONTROL-API, research
├── third_party/     # Vendored dependencies (rnnoise)
├── *.bat            # start/stop mixer, TUI/GUI launchers, autostart install
├── CMakeLists.txt
└── README.md
```

---

## Roadmap Summary

| Phase | Goal | Status |
|---|---|---|
| 0 | Research — proof of concept for every major component | Done |
| 1 | Virtual driver + WASAPI routing — audio flows through AnniAudio | Engine + loopback working; driver unsigned |
| 2 | DSP chain + routing matrix — EQ, RNNoise, per-app routing, scenes, control API, Loupedeck | Working end-to-end |
| 3 | Spatial audio — HRTF convolution | POC + engine done; mixer integration next |
| 4 | API + hotkeys + CLI | Control API + TUI + Loupedeck done; global hotkeys planned |
| 5 | UI — graphical mixer | Planned |
| 6 | Installer, driver signing, packaging | Planned |

Full detail in `docs/ROADMAP.md`.

---

## License

MIT — open source, use it freely.

If this is used commercially, consider contributing back.
