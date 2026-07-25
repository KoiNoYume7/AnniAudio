# AnniAudio — Code / Doc Discrepancy Report (Phase 2)

> Scope: C++ core, Python TUI, C# plugin, control API, and project operating docs.
> No code changes were made for this report.

## 1. The first question: is the per-(input × output) strip duplication deliberate or drift?

**Answer: it was a deliberate design decision, but it has aged into the project's most expensive technical-debt item. Treat it as drift for prioritisation.**

### Evidence

The duplication is not accidental. It was introduced when `AudioMixerMatrix` was created to add any-to-any routing on top of the existing `AudioMixer` engine:

- Commit `e451d09` introduced `AudioMixer` as a single-output, multi-source mixer.
- Commit `6bfbd28` (`feat: true any-to-any mixer matrix with per-route levels and patchbay GUI`) added `AudioMixerMatrix` and explicitly documented the trade-off in the first version of `AudioMixerMatrix.hpp`:

  > "Internally this currently owns one AudioMixer per output. Sources routed to multiple outputs are opened once per output; this is simple and matches the existing per-output mixer design. A future optimisation can share capture across outputs if that becomes necessary."

- The current `AudioMixerMatrix.hpp` still labels this "Stage 1" and says "A future shared group bus will collapse that" (`src/core/AudioMixerMatrix.hpp`, lines 107-113).
- The structure in `src/core/AudioMixerMatrix.cpp` implements exactly that: `rebuildGroupRoutesLocked` (lines 934-970) loops `for (InputId iid : g.inputIds) { for (const auto& outName : g.outputIds) { ... mixer->addStrip(cfg); } }`.
- `AudioMixer::setupStripDsp` (lines 371-446) instantiates a full per-strip DSP chain: `NoiseSuppressor`, `EqChain`, two `Spatializer` instances plus a `colorEq`.

So the authors knew what they were doing and left a clear breadcrumb. It is not a case of "multi-output was bolted onto a single-output engine and nobody noticed." However, the later addition of per-input RNNoise, voice EQ, and especially per-input HRTF (commits `b2f1166` and `9ddc6c1`) made the cost far higher than the original comment anticipated.

### What it costs

Fixing it means moving to a shared group bus: capture each input once, run its DSP once, hold a processed (mono or stereo) group bus, then mix that bus to each output with per-output send gains and format conversion. `AudioMixer` would have to accept pre-processed sources or a new `GroupBus` stage would need to sit between input capture and per-output mixing.

**Risk:** high. The audio thread, WASAPI event handles, resampler state, and HRTF convolution state are all tied to the current per-route `Strip` model. A shared bus must still handle per-output format differences cleanly — and HRTF is currently resampled and convolved at the *output* rate inside each strip. Keeping that behavior while sharing the source capture is non-trivial.

**Value:** very high for any real multi-output setup. A microphone with denoise + voice EQ + HRTF sent to three outputs currently runs three RNNoise instances, three EQ chains, and six HRTF convolutions.

**Verdict:** deliberate debt, now the highest-value refactor.

---

## 2. Discrepancies ranked by value ÷ risk

### 2.1 `/api/state` and `/api/events` omit `controlPort` despite the docs

**What it is:** `docs/MIXER-CONTROL-API.md` describes `GET /api/state` as returning `{ running, controlPort, inputs, groups, outputs }` and the `MixerStateSnapshot` struct in `src/core/AudioMixerMatrix.hpp` (line 101) includes `uint16_t controlPort = 8850`. In `src/core/MixerControlServer.cpp` the server's own `stateJson()` (lines 106-118) builds the JSON by hand and forgets to set `j["controlPort"]`. The same `stateJson()` is used for SSE `/api/events`.

**Why it looks like drift:** `AudioMixerMatrix` already computes `controlPort` in `snapshotNoLock()` and `AudioMixerMatrix::toJson()` (lines 30-33) includes it. The HTTP server duplicated the serialisation and left the field out.

**Cost to change:** one line — either call `AudioMixerMatrix::toJson(snap)` instead of hand-rolling JSON, or add `j["controlPort"] = snap.controlPort;`.

**Risk:** very low. Adding a field is backward-compatible for clients that ignore it. TUI and plugin do not consume `controlPort` today.

### 2.2 `GET /api/applications` returns more fields than documented

**What it is:** The `ApplicationInfo` JSON emitted by `src/core/MixerControlServer.cpp` (lines 90-104) contains `processId`, `name`, `displayName`, `windowTitle`, `endpoint`, `isInput`, `isActive`, `isMuted`, `volume`, and `isSystem`. `docs/MIXER-CONTROL-API.md` only documents `process id, name, endpoint, mute, volume, isActive, windowTitle`.

**Why it looks like drift:** `scripts/mixer_tui.py` already relies on `isInput` and `isSystem` (lines 594, 667, 1220), so the fields are load-bearing but undocumented. `displayName` appears to be unused by any shipped client.

**Cost to change:** update the API doc to list every field, or trim `toJson(const ApplicationInfo&)` to match the doc. If trimming, check the plugin and TUI first.

**Risk:** low for a doc fix; low-medium for removing fields.

### 2.3 `DELETE /api/inputs/{id}` and `DELETE /api/groups/{id}` skip COM initialisation

**What it is:** Most mutating HTTP handlers in `src/core/MixerControlServer.cpp` call `EnsureComInitializedOnThisThread()`. `DELETE /api/inputs/{id}` (line 461) and `DELETE /api/groups/{id}` (line 558) do not. When the mixer is not running, `matrix.removeInput()` / `removeGroup()` can synchronously tear down a strip (`AudioMixer::teardownStrip`), which stops and releases COM audio clients. Without `CoInitializeEx` on the httplib worker thread, that teardown is undefined.

**Why it looks like drift:** The `DELETE` routes were added without the same COM guard that every `POST`/`PATCH` route uses.

**Cost to change:** add `EnsureComInitializedOnThisThread();` at the top of both handlers.

**Risk:** very low. The helper is idempotent per thread.

### 2.4 `mixBuf` is allocated on the first render callback

**What it is:** `src/core/AudioMixer.cpp` `Impl::processRender()` (line 917) calls `if (mixBuf.size() < outSamples) mixBuf.resize(outSamples);` on the real-time audio thread. `mixBuf` is not pre-sized in `openOutput()` (lines 245-299) even though `renderBufFrames` and `renderCh` are known once the output is initialised. `readBuf` per strip *is* preallocated in `finalizeStripBuffers()` (line 510), but `processRender()` still defensively resizes it (line 922).

**Why it looks like drift:** It contradicts `CLAUDE.md` and `src/core/AudioMixer.hpp` comments that state all allocation happens before the strip reaches the audio thread.

**Cost to change:** pre-size `mixBuf` in `openOutput()` to `renderBufFrames * renderCh`; replace the remaining hot-path `.resize()` calls with `assert()` or `reserve()` where the bound is known.

**Risk:** low. If an assert ever fires, it reveals a real buffer-size assumption violation rather than hiding it.

### 2.5 `AudioMixer.hpp` comment claims HRTF collapses to mono

**What it is:** `src/core/AudioMixer.hpp` lines 31-33 describe `spatial` as "downmixed to mono and convolved to a positioned stereo image". The actual implementation in `src/core/AudioMixer.cpp` (`writeStripSpatial`, lines 864-903) keeps the left and right source channels and convolves each as its own virtual speaker at `azimuth ± kSpatialSpreadDeg`, which is stereo-preserving. The `Strip` struct comment at lines 102-106 is correct, but the `MixerStripConfig` comment above it is stale.

**Why it looks like drift:** The mono comment predates the stereo-preservation fix (commit `d0a06ed` "fix: HRTF spatial preserves the stereo image") and was not updated.

**Cost to change:** update the `MixerStripConfig.spatial` comment to match `writeStripSpatial` and the rest of the docs.

**Risk:** none.

### 2.6 `CLAUDE.md` still treats HRTF and `Spatializer` as not shipped

**What it is:** `CLAUDE.md` "Roadmap / next candidates" lists "HRTF into the mixer" as a next candidate and describes `src/dsp/` as only `EqChain` and `NoiseSuppressor`. Per-input HRTF is already implemented (`src/dsp/spatializer.{hpp,cpp}`, `AudioMixer.cpp` `writeStripSpatial`, `ROADMAP.md` Phase 3 Stage C done).

**Why it looks like drift:** The operating manual was not updated after commits `9ddc6c1`, `d0a06ed`, and `f6161a9`.

**Cost to change:** update `CLAUDE.md` to mark HRTF done and include `Spatializer` in the `src/dsp/` bullet.

**Risk:** none, but `CLAUDE.md` is the operating manual, so it should be deliberate.

### 2.7 `AudioEngine` / `route_cli process` is a legacy duplicate engine

**What it is:** `src/core/AudioEngine.cpp` is only used by `route_cli` commands `list`, `process`, `process-eq`, `route`, and `default` (`src/core/route_cli.cpp`, lines 83, 197, 266, 320). The mixer path uses `AudioMixer`/`AudioMixerMatrix`; the control API never exposes `process` functionality. `AudioEngine` duplicates WASAPI capture/render, format conversion, ring buffer, volume, and real-time thread logic. It does not autosave state or support the matrix model.

**Why it looks like drift:** `AudioEngine` was the first working engine (`d8558bf` "AudioEngine routing engine — Phase 1 first working audio path"). `AudioMixer` and `AudioMixerMatrix` superseded it, but the legacy `process` / `route` CLI commands were never migrated. The mixer can already do one-input/one-output routing with a config.

**Cost to change:** medium. Options: (a) reimplement `process`/`route` by generating a temporary `config/mixers/*.json` and using `AudioMixerMatrix`; (b) remove `process`/`process-eq`/`route` and update `README.md` and `scripts/restore-default.ps1` to use mixer features; (c) keep `list` and `default` but replace the underlying endpoint enumeration with `AudioMixerMatrix` or raw `MMDeviceEnumerator`.

**Risk:** medium. These commands are documented in `README.md` "Daily use without the driver" and are convenient for quick loopback tasks.

### 2.8 Hot-path `std::vector::resize` calls in `AudioMixer` and `AudioEngine`

**What it is:** `AudioMixer::processStrip()` uses defensive `resize()` calls on `captureTmp` (line 820), `chL`/`chR` (lines 809, 838), `resL`/`resR` (line 871), and `convertBuf` inside `writeStripSpatial` (line 895). `AudioEngine::runThread()` resizes `captureTmp` (line 109) and `renderTmp` (line 152) every capture/render event. Most are no-ops after warm-up, but they are still allocation calls on the real-time thread.

**Why it looks like drift:** They are defensive guards that were not replaced with `reserve()` + `assert()` once buffer bounds were known. `AudioEngine` is worse: it does not pre-size at all.

**Cost to change:** low for `AudioMixer` (pre-size or assert); medium for `AudioEngine` because it is easier to fold `AudioEngine` into `AudioMixerMatrix` than to harden it.

**Risk:** low for `AudioMixer` asserts; medium for `AudioEngine` because it is legacy and less tested.

### 2.9 Naming inconsistency: `Strip` vs `Input`/`Group`/`Cable`

**What it is:** `AudioMixer` uses `Strip`, `StripId`, `MixerStripConfig`, `StripSnapshot`, `StripSourceType` (`src/core/AudioMixer.hpp`). `AudioMixerMatrix`/API/TUI use `Input`/`InputConfig`/`InputSnapshot`, `Group`, and `Output`. The TUI pane label is "Virtual Cables", the plugin calls groups "Cable", and `MixerStripConfig` has `knobIndex` while the public model has `knobIndex` on the group. The same logical entity (an input routed to a specific output) is a `Strip` internally, an `Input` externally, and a source/app in the TUI.

**Why it looks like drift:** `AudioMixer` predates the matrix model and its vocabulary was not aligned when `AudioMixerMatrix` was added.

**Cost to change:** low if only documented; medium if renamed. Renaming `Strip` to `Route` or `InputRoute` is mechanical but touches `AudioMixer.cpp/hpp`, `AudioMixerMatrix.cpp`, `MixerControlServer.cpp`, and the DSP comments.

**Risk:** low for documentation; low-medium for a rename refactor.

### 2.10 `route_cli` command overlap (`process` vs `route`)

**What it is:** Both `route_cli process` and `route_cli route` call `cmdRoute()` (`src/core/route_cli.cpp`, lines 661 and 701). `process-eq` is `cmdProcessEq()` with a hardcoded EQ. The mixer can already express all three with a JSON config and the `mixer` command.

**Why it looks like drift:** Commands grew organically; `route` was added later but `process` was not narrowed.

**Cost to change:** medium. Deprecate `process-eq` and make `process` a thin wrapper around `mixer` with a one-input/one-output config, or remove `process`/`route` entirely.

**Risk:** medium due to CLI compatibility.

### 2.11 `MIXER-CONTROL-API.md` response bodies are underspecified

**What it is:** The endpoint table lists request bodies and behaviour but does not document response shapes for `POST /api/groups`, `POST /api/outputs`, `POST /api/outputs/master`, `PATCH /api/inputs/{id}`, `POST /api/inputs/{id}/direction`, `POST /api/scenes/save`, etc.

**Why it looks like drift:** The doc was written from a human-readable angle rather than as a machine schema, and it has not kept up with the exact JSON returned by `MixerControlServer.cpp`.

**Cost to change:** low. Add response examples to the table.

**Risk:** none.

---

## 3. Summary ranking

| Rank | Item | Value | Risk | Fix now? |
|------|------|-------|------|----------|
| 1 | Per-(input × output) strip duplication / shared group bus | Very high | High | Plan/refactor, not a quick fix |
| 2 | `/api/state` missing `controlPort` | High | Very low | Yes |
| 3 | `/api/applications` undocumented fields | Medium-High | Low | Yes (doc fix) |
| 4 | `DELETE` routes missing COM init | Medium | Very low | Yes |
| 5 | `mixBuf` allocated on first render callback | Medium | Low | Yes |
| 6 | HRTF mono-downmix comment in `AudioMixer.hpp` | Low | None | Yes |
| 7 | `CLAUDE.md` still lists HRTF as future | Medium | None | Yes |
| 8 | `AudioEngine` / `route_cli process` legacy engine | High | Medium | Plan for deprecation |
| 9 | Hot-path `resize()` calls | Medium | Low-Medium | Yes for `AudioMixer`; fold `AudioEngine` |
| 10 | `Strip` vs `Input`/`Group`/`Cable` naming | Low | Low | Document or rename |
| 11 | `route_cli process`/`route` overlap | Low-Medium | Medium | Deprecate when `process` is folded |
| 12 | API response shapes underspecified | Low | None | Doc fix |

The single highest-value architectural change is the shared group bus. The safest immediate wins are items 2-7: small doc or code fixes that remove contradictions without touching the audio thread model.

---

## 4. Appendix: tree-walk follow-up

Additional items surfaced by checking directories that are not described in `ARCHITECTURE.md` or `CLAUDE.md`.

### A.1 `src/api/` and `src/routing/` are empty placeholders *(resolved in cleanup pass)*

**What it is:** Both directories contained only `.gitkeep` and have been deleted. They are not referenced in any `CMakeLists.txt`. The REST+SSE API server is in `src/core/MixerControlServer.cpp` and the routing matrix is in `src/core/AudioMixerMatrix.cpp`.

**Why it looks like drift:** They were reserved during early design but never populated. Their existence suggests unfinished or planned modules.

**Cost to change:** document them as reserved, or delete them if they will not be used.

**Risk:** none.

### A.2 `cli/anniaudio.ps1` is not the Phase 4 CLI and had dead subcommands *(resolved in cleanup pass)*

**What it is:** `cli/anniaudio.ps1` is a PowerShell driver/signing control panel (`install`, `uninstall`, `dev-mode`, `gaming-mode`, `config`, `status`, `build`, `route`, `tui`, `restore-default`). `ROADMAP.md` Phase 4 lists a separate "Standalone CLI client (`anniaudio-cli`) wrapping the API" that does not exist. The `gui` subcommand has been removed; the `tui` subcommand now launches `mixer-tui.bat`.

**Why it looks like drift:** The `gui` subcommand and the old `tui` path were stale from an earlier web-GUI experiment, and the directory name `cli/` is ambiguous given the Phase 4 plan.

**Cost to change:** done — `gui` removed and `tui` repointed. Remaining: clarify that `cli/` is the driver/signing panel, not the Phase 4 API client.

**Risk:** low.

### A.3 `tests/` contents and status

**What it is:**
- `test_routing.cpp` (`tests/CMakeLists.txt`, line 24) is a manual 5-minute harness that uses the legacy `AudioEngine` to route a capture endpoint to a render endpoint and print frame counts. It has no automatic assertions.
- `test_mixer_live_edit.cpp` (`tests/CMakeLists.txt`, line 27) is automated: it uses `check()` macros and exits 1 on failure. It tests live add/remove/rename/volume of `AudioMixer` strips while the mixer is running.
- `test_httplib.cpp` is referenced in neither `tests/CMakeLists.txt` nor the source tree.

Both listed tests build successfully (`cmake --build build --config Release --target test_routing test_mixer_live_edit`), and `test_mixer_live_edit` passes when run against real endpoints:

```text
test_mixer_live_edit.exe "Speakers (Realtek(R) Audio)" \
                       "Microphone (Creative Live! Cam Sync V3 Mic)" \
                       "Microphone (2- HyperX QuadCast S)"
# output: ALL CHECKS PASSED (0 failures)
```

It is a real regression net for `AudioMixer` strip lifecycle but not for `AudioMixerMatrix`, group routing, or HRTF/DSP.

**Why it looks like drift:** `test_routing` tests the legacy `AudioEngine` rather than the mixer matrix, and no test covers `AudioMixerMatrix`. `test_httplib` appears to have been planned and abandoned.

**Cost to change:** low to document what exists; medium to add `AudioMixerMatrix` integration/regression tests.

**Risk:** low for documentation; medium for new tests because they need real or mocked WASAPI endpoints.

### A.4 `driver/build/` was not in `.gitignore`

**What it is:** `.gitignore` ignored `build/`, `driver/x64/`, and `driver/x86/`, but not `driver/build/`. A WDK MSBuild that produced `driver/build/` would be tracked by git.

**Why it looks like drift:** `driver/build/` is the natural sibling to `build/`; the omission was an oversight.

**Cost to change:** one line in `.gitignore`.

**Risk:** none. (Fixed during this investigation by adding `driver/build/` to `.gitignore`.)
