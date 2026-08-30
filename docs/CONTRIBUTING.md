# AnniAudio — Commit Convention and Workflow

---

## Commit Format

```
<type>(<scope>): <short description>

[optional body]
```

**Keep the subject line under 72 characters.**

---

## Types

| Type | When to use |
|---|---|
| `docs` | Documentation only — README, ARCHITECTURE, ROADMAP, etc. |
| `poc` | Phase 0 proof-of-concept code — standalone test harnesses |
| `feat` | New production feature |
| `fix` | Bug fix |
| `refactor` | Code change that neither fixes a bug nor adds a feature |
| `test` | Adding or updating tests |
| `chore` | Build system, .gitignore, CI, dependencies, tooling |

---

## Scopes

| Scope | What it covers |
|---|---|
| `phase0` | Phase 0 planning artifacts |
| `driver` | Virtual WDM driver (`driver/`) |
| `core` | WASAPI mixer and audio engine (`src/core/`) |
| `matrix` | Routing matrix and `AudioMixerMatrix` |
| `dsp` | DSP chain, EQ, and spatializer (`src/dsp/`) |
| `tui` | Curses TUI (`scripts/mixer_tui.py`, `scripts/tui_*.py`) |
| `plugin` | Loupedeck C# plugin (`AnniAudioMixerPlugin/`) |
| `config` | Config system (`config/` + loading code) |
| `hotkeys` | Global hotkey engine (`config/hotkeys.json`; shipped) |
| `ui` | Graphical UI (Electron stub in `gui/`; TUI and Loupedeck are current) |
| `installer` | Installer and packaging (future; technology not yet chosen) |
| `ci` | GitHub Actions |
| `deps` | Third-party dependencies |
| `tests` | POC and verification tools (`tests/`) |

---

## Examples

```
poc(dsp): biquad EQ shapes frequency response correctly
poc(noise): RNNoise reduces noise on test signal at 48kHz
poc(wasapi): loopback capture working on default output device
feat(driver): virtual WDM device appears in Windows device list
fix(dsp): precision loss in biquad coefficients at <100Hz
docs(phase0): finalize REST API schema with request/response bodies
chore(ci): add GitHub Actions build workflow for CMake
```

---

## Phase Tags

A tag is created at the completion of each significant milestone:

| Tag | Meaning |
|---|---|
| `phase0-planning` | All schemas, decisions, and skeleton committed |
| `phase0-complete` | All Phase 0 POCs passing, ready to build |
| `phase1-complete` | Virtual driver builds; WASAPI routing works with loopback |
| `phase2-complete` | DSP chain (EQ + RNNoise), routing matrix, control API + SSE, TUI, Loupedeck plugin working |
| `phase3-complete` | Per-input HRTF spatialization live in the mixer |
| `phase4-complete` | Global hotkeys, standalone CLI client (`anniaudio-cli`) |
| `phase5-complete` | Graphical UI (Electron or other chosen stack) complete |
| `phase6-complete` | Installer and packaging complete |

Tag command:
```powershell
git tag -a "phase0-complete" -m "Phase 0 complete — all POCs verified"
git push origin --tags
```

---

## Build and Run Tests

Tests are not built by default. Configure with `-DBUILD_TESTS=ON`:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTS=ON
cmake --build build --config Release
```

This produces Phase-0 POCs and verification binaries in `build/bin/Release/`:

| Binary | Type | What it checks | How to run |
|---|---|---|---|
| `poc_eq.exe` | Manual/listening | Biquad EQ frequency response | Run and read output |
| `poc_rnnoise.exe` | Manual/listening | RNNoise suppression on test signal | Run and listen |
| `poc_wasapi.exe` | Manual/listening | 1 kHz sine loopback capture | Run and listen |
| `poc_hrtf.exe` | Manual/listening | HRTF convolution against MIT KEMAR; renders `hrtf_orbit_48k.wav` | Run from repo root so `assets/hrtf/mit_kemar.sofa` resolves |
| `test_input_processor.exe` | Manual harness | Capture a single endpoint and run per-input DSP | `test_input_processor.exe [capture_hint]`; stop with Ctrl+C |
| `test_new_pipeline.exe` | Manual harness | Verify `InputProcessor → GroupBus → OutputMixer` routing | `test_new_pipeline.exe [capture_hint] [render_hint]`; stop with Ctrl+C |
| `test_mixer_matrix.exe` | Manual harness | Load and run an `AudioMixerMatrix` config end-to-end | `test_mixer_matrix.exe config/mixers/default.json`; stop with Ctrl+C |
These are manual harnesses, not automated tests. They require real audio endpoints. None are wired into CTest. The legacy `test_routing.exe` and `test_mixer_live_edit.exe` were removed with the old `AudioEngine`/`AudioMixer` code.

---

## Branch Strategy

- `dev` — active development branch. All work lands here.
- `main` — kept close to releasable, but phase-boundary tags are cut from `dev`.
- Use short-lived branches for risky experiments or reviewable chunks, then merge into `dev`.
- PRs are welcome on `dev`.
