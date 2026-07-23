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
| `hotkeys` | Hotkey engine |
| `ui` | Graphical UI (future; currently TUI / Loupedeck) |
| `installer` | NSIS/WiX installer and packaging |
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
| `phase1-complete` | Virtual driver + WASAPI routing working end-to-end |
| `phase2-complete` | DSP chain (EQ + noise cancellation) working |
| `phase3-complete` | Spatial audio / HRTF working |
| `phase4-complete` | REST API + hotkeys + CLI working |
| `phase5-complete` | Electron UI complete |
| `phase6-complete` | Installer and packaging complete |

Tag command:
```powershell
git tag -a "phase0-complete" -m "Phase 0 complete — all POCs verified"
git push origin --tags
```

---

## Branch Strategy

- `dev` — active development branch. All work lands here.
- `main` — kept close to releasable, but phase-boundary tags are cut from `dev`.
- Use short-lived branches for risky experiments or reviewable chunks, then merge into `dev`.
- PRs are welcome on `dev`.
