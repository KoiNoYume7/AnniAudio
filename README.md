# AnniAudio

One install. One config. Full control over your audio.

A system-wide audio processing suite for Windows 11. Ships its own virtual WDM audio driver, a full parametric EQ, GPU-accelerated noise cancellation, and HRTF-based spatial audio — all in a single background process with a REST API, configurable hotkeys, and a routing matrix that lets you wire audio wherever you want.

Built because SteelSeries Sonar gets close but breaks constantly, has no API, and still isn't flexible enough. And because every other good audio tool is either fragmented across three apps or locks you in with no way out.

Part of the [Anni Ecosystem](https://github.com/KoiNoYume7).

---

## Status

**Planning / Research phase**

See [docs/ROADMAP.md](docs/ROADMAP.md) for phase breakdown and [docs/RESEARCH.md](docs/RESEARCH.md) for gathered intel.

---

## Features

### Virtual Audio Routing
- Ships its own virtual WDM audio driver — no dependency on VB-Cable or any other third-party software
- Create and destroy virtual cables on demand
- Full routing matrix — any source to any destination
- Per-app routing (route Discord to a different cable than your game)
- Loopback support

### Noise Cancellation
- NVIDIA RTX Effects SDK — GPU-accelerated, same engine as RTX Voice, exposed as a developer API
- RNNoise fallback for non-NVIDIA systems
- Adjustable suppression threshold
- Swappable backends behind a single interface

### Parametric EQ
- Full biquad parametric EQ per audio stream
- Filter types: peak, low shelf, high shelf, low-pass, high-pass, notch
- Preset save and load
- Per-cable EQ chains

### Spatial Audio
- HRTF-based 3D audio via FFT convolution
- Multiple free HRTF datasets bundled (MIT KEMAR default)
- Supports any SOFA-format HRTF file
- Headphone-optimized output

### Control and Automation
- REST API — control everything programmatically
- WebSocket event stream for real-time state (level meters, device changes)
- Global hotkeys, fully rebindable
- CLI client wrapping the API
- Single JSON config file — restart the service and everything restores

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

Full technical breakdown in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Tech Stack

| Layer | Technology |
|---|---|
| Platform | Windows 11 |
| Core engine | C++ (WASAPI) |
| Virtual driver | WDM / PortCls — bundled, based on Microsoft sysvad sample |
| DSP / EQ | C++ — custom biquad filter chain |
| Noise cancellation | NVIDIA RTX Effects SDK (models downloaded on first run) + RNNoise fallback |
| Spatial audio | HRTF convolution via libmysofa + KissFFT |
| API server | C++ — cpp-httplib (header-only) |
| Config | JSON — nlohmann/json |
| Build | CMake |
| UI | Electron |

---

## Project Structure

```
AnniAudio/
├── src/
│   ├── core/        # Engine, session management
│   ├── driver/      # Virtual WDM driver source
│   ├── dsp/         # EQ, biquad filters, DSP utilities
│   ├── spatial/     # HRTF loader, convolution engine
│   ├── noise/       # Noise cancellation wrappers
│   ├── routing/     # Routing matrix logic
│   └── api/         # REST + WebSocket server
├── include/         # Public headers
├── docs/            # Architecture, roadmap, research
├── tests/           # Unit and integration tests
├── third_party/     # Vendored dependencies
├── assets/          # Icons, bundled HRTF datasets
├── scripts/         # Build helpers, signing scripts
├── CMakeLists.txt
└── README.md
```

---

## Roadmap Summary

| Phase | Goal |
|---|---|
| 0 | Research — proof of concept for every major component |
| 1 | Virtual driver + WASAPI routing — audio flows through AnniAudio |
| 2 | DSP chain — EQ and noise cancellation working |
| 3 | Spatial audio — HRTF convolution |
| 4 | API + hotkeys + CLI |
| 5 | UI |
| 6 | Installer and packaging |

Full detail in [docs/ROADMAP.md](docs/ROADMAP.md).

---

## Driver Signing

For personal use, enable Windows test signing mode and use a self-signed certificate:

```powershell
bcdedit /set testsigning on
# Reboot required
```

For distribution, an EV code signing certificate is required (~$300-500/year). Attestation signing via the Microsoft Hardware Dev Center is the free alternative.

---

## License

MIT — open source, use it freely.

If you use this commercially, consider contributing back.

---

*Built late at night with energy drinks and genuine frustration at the state of Windows audio tooling.*