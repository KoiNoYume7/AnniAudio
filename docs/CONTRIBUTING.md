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
| `driver` | Virtual WDM driver (`src/driver/`) |
| `wasapi` | WASAPI audio engine (`src/core/`) |
| `routing` | Routing matrix (`src/routing/`) |
| `dsp` | DSP chain and EQ (`src/dsp/`) |
| `noise` | Noise cancellation (`src/noise/`) |
| `spatial` | HRTF / spatial audio (`src/spatial/`) |
| `api` | REST + WebSocket API server (`src/api/`) |
| `config` | Config system |
| `hotkeys` | Hotkey engine |
| `ui` | Electron frontend |
| `installer` | NSIS/WiX installer and packaging |
| `ci` | GitHub Actions |
| `deps` | Third-party dependencies |

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

- `main` — always releasable, tagged at phase boundaries
- Feature work is done directly on `main` during early phases (no PRs required until Phase 4+)
- If a POC needs risky experimentation, use a short-lived branch: `poc/driver-sysvad`, etc.
