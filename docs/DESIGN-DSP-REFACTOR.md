# AnniAudio — Per-Input DSP Refactor Design

**Status:** implemented and running in `route_cli mixer` (Stage 1–3 complete; Stage 4 cleanup and final merge pending)  
**Author:** Devin (with project context from `CLAUDE.md` and `docs/ARCHITECTURE.md`)

---

## 1. Problem

Today the routing matrix creates one `AudioMixer::Strip` for every `(input × output)` route. A `Strip` owns the full capture-side DSP chain:

- `NoiseSuppressor` (RNNoise)
- `EqChain` (voice EQ)
- `Spatializer` left + right (HRTF)
- `colorEq` (HRTF color compensation)

So a microphone routed to three outputs runs:

- 3× RNNoise instances
- 3× EQ chains
- 6× HRTF spatializers (left/right per output)

This is the dominant CPU cost in the mixer and the biggest source of duplicated work. It also makes the architecture harder to reason about: an input's processed signal is not a single artifact; it is recomputed independently for every output.

## 2. Goal

Move DSP to a **single capture-side pass per input**. The processed input is then mixed into every group/output it feeds. The user-visible behavior (routing, volumes, mutes, per-output send gains, scenes, API) stays the same.

## 3. Non-goals

- This design does **not** change the control API or config schema.
- It does **not** remove the per-output `AudioMixer` concept; it splits it into an `OutputMixer` that only sums and renders.
- It does **not** introduce a plugin/DLL system or VST hosting.

## 4. Current architecture (simplified)

```
input A ──► Strip(A->out1) ──► output 1
        ──► Strip(A->out2) ──► output 2
        ──► Strip(A->out3) ──► output 3

Strip = capture + denoise + EQ + spatial + volume/mute + ring buffer
AudioMixer per output = sum strips + master volume/mute + WASAPI render
AudioMixerMatrix = inputs/groups/outputs/routes map + per-strip state
```

Each `Strip` captures from the source endpoint or process loopback, resamples to the **output** sample rate, then runs DSP at that rate. Volume/mute is atomic per strip.

## 5. Proposed architecture

```
input A ──► InputProcessor(A) ──► ProcessedInput(A)
input B ──► InputProcessor(B) ──► ProcessedInput(B)

ProcessedInputs ──► GroupBus("Music") ──► per-output send gains ──► OutputMixer(out1)
                                             └──► OutputMixer(out2)
                                             └──► OutputMixer(out3)
```

### 5.1 Components

| Component | Responsibility |
|---|---|
| `InputProcessor` | One per `InputConfig`. Captures the source once, runs denoise/EQ/spatial at a common processing rate, exposes a `ProcessedInput` ring buffer at that rate. |
| `GroupBus` | One per (group, output) pair. Sums the `ProcessedInput` streams of its member inputs. Applies group volume, mute, and the send gain for that output. Produces an interleaved stream at 48 kHz. |
| `OutputMixer` | One per output. Sums group contributions and resamples to the output rate. Applies output master volume/mute and renders to WASAPI. |

### 5.2 Data flow

1. **Capture thread / input processor thread** wakes on capture event.
2. Read packets from `IAudioCaptureClient`.
3. Resample to a common processing rate if needed.
4. Run RNNoise (48 kHz) → EQ → optional HRTF spatialization.
5. Write processed stereo interleaved frames to `ProcessedInput::ring`.
6. **Render thread / output mixer thread** wakes on render event.
7. For each group connected to this output, call `GroupBus::mix()` into a scratch buffer.
8. Sum, apply master, clamp, release WASAPI buffer.
9. **Group bus mixing is output-driven.** Each `GroupBus` is owned by `AudioMixerMatrix` and called directly from the `OutputMixer` render thread. This avoids extra worker threads and keeps latency low; the trade-off is that each output re-sums the same group inputs, which is cheap compared to the per-input DSP savings.

## 6. Sample-rate strategy

The current mixer resamples per strip to each output's rate. In the proposed design we need a **single processing rate** for shared DSP, otherwise HRTF/EQ/denoise would still be duplicated per output rate.

### 6.1 Candidate: fix processing at 48 kHz stereo

Reasoning:

- RNNoise is already trained at 48 kHz and is skipped at other rates today.
- `assets/hrtf/mit_kemar.sofa` and `libmysofa` are easy to run at 48 kHz.
- Most Windows endpoints support 48 kHz natively; 44.1 kHz endpoints would receive a final resample 48→44.1, which is cheap and high quality for the final mix.
- 96 kHz/192 kHz endpoints still work: DSP at 48 kHz, output resamples to the higher rate.

### 6.2 Input side

- Capture at the source rate (often 48 kHz).
- If `captureRate != 48000`, resample once to 48 kHz before denoise/EQ.
- If `captureRate == 48000`, no input resample needed.

### 6.3 Output side

- Group bus produces 48 kHz stereo frames.
- `OutputMixer` resamples from 48 kHz to `renderRate` if needed, after applying group volume/mute and send gains.

### 6.4 Spatial audio

- HRTF convolution runs at 48 kHz, producing a 48 kHz stereo binaural stream.
- If the output is >2 channels, the binaural pair is placed on channels 0/1 and the rest are zero (same as today).
- If the output is 44.1 kHz, the binaural result is resampled to 44.1 kHz at the output stage. The spatial cues are preserved well enough for consumer use.

## 7. Threading model

### 7.1 Input processor thread

- One thread per `InputProcessor`.
- Owns its `IAudioClient`, capture event, capture buffers, DSP state.
- Writes to a single-producer, multi-reader `MultiReaderRingBuffer` consumed by group buses.
- Volume/mute/denoise/EQ/spatial/azimuth/elevation are atomic toggles read each callback.

### 7.2 Group bus mixing (inline in output mixer thread)

- `GroupBus::mix()` is called directly from each `OutputMixer` render callback.
- The bus reads from each input's `MultiReaderRingBuffer` with its own cursor,
  sums, applies group volume/mute and the per-output send gain, and writes to a
  caller-provided scratch buffer.
- No extra worker thread; each output re-sums the same group inputs, which is cheap
  compared to the saved per-input DSP work.

### 7.3 Output mixer thread

- One thread per `OutputMixer`.
- Waits on render event.
- Snapshots its list of `GroupBus` instances, calls `GroupBus::mix()` into a scratch
  buffer for each, sums, resamples to `renderRate` if needed, applies master, clamps,
  renders.

### 7.4 Lock-free volume/mute

- `InputProcessor` reads `input.volume`, `input.muted`, `input.spatial`, `input.azimuth`, `input.elevation` from atomics each callback.
- `GroupBus` reads `group.volume`, `group.muted`, and `group.outputGains` from atomics each callback.
- `OutputMixer` reads `output.master` and `output.muted` from atomics.

## 8. Control API / persistence impact

None. The `InputConfig`, `GroupConfig`, `OutputSnapshot`, and `MixerStateSnapshot` schemas remain unchanged. `AudioMixerMatrix` continues to expose `addInput`, `removeInput`, `updateInput`, `setInputDirection`, `addGroup`, `removeGroup`, `setGroupVolume`, etc.

The internal mapping changes:

- `inputs` map stays.
- `groups` map stays.
- `outputs` map now owns `std::unique_ptr<OutputMixer>`.
- New `inputProcessors` map owns `std::unique_ptr<InputProcessor>`.
- New `groupBuses` map owns `std::unique_ptr<GroupBus>`.
- `routes` map can be removed; connectivity is implicit from `group.inputIds` and `group.outputIds`.

## 9. Migration plan (staged)

### Stage 0: Design verification (this doc)

- Review sample-rate strategy.
- Decide group-bus threading option.
- Identify resampler quality requirements.

### Stage 1: Add new abstractions alongside the old ones

- [x] Implement `InputProcessor` class in `src/core/InputProcessor.{hpp,cpp}`.
- [x] Capture from device or application loopback, resample to 48 kHz, run denoise/EQ/spatial/colorEq, write to `MultiReaderRingBuffer`.
- [x] Implement `GroupBus` class in `src/core/GroupBus.{hpp,cpp}`.
- [x] Mix processed inputs, apply group volume/mute/send gains atomically, called from `OutputMixer`.
- [x] Implement `OutputMixer` class in `src/core/OutputMixer.{hpp,cpp}`.
- [x] Sum group contributions, resample from 48 kHz to `renderRate`, apply master and render.
- [x] Add verification tests: `tests/test_input_processor.cpp`, `tests/test_new_pipeline.cpp`.

### Stage 2: New matrix backend

- [x] Refactor `AudioMixerMatrix` to use `InputProcessor`, `GroupBus`, and `OutputMixer`.
- [x] Map `InputConfig` → `InputProcessor`.
- [x] Map `GroupConfig` → per-(group,output) `GroupBus`.
- [x] Map `OutputConfig` → `OutputMixer`.
- [x] Hook all control API methods to the new backend.

### Stage 3: Feature parity + A/B testing

- [x] `route_cli mixer` runs the new pipeline against the user's real `config/mixers/main.json`.
- [x] Live state and control via `/api/state` and `PATCH /api/groups/{id}` verified.
- [x] `tests/test_mixer_matrix.cpp` exercises add/output/input/group/start/snapshot/stop.
- [ ] Full TUI and Loupedeck plugin session.
- [ ] CPU comparison with the old `AudioMixer`/`Strip` path.

### Stage 4: Remove legacy `AudioMixer`/`Strip` path

- [ ] Delete `AudioMixer.cpp/hpp` and strip-based route logic from `AudioMixerMatrix`.
- [ ] Remove or port `tests/test_mixer_live_edit.cpp`.
- [ ] Decide whether to rename `OutputMixer` → `AudioMixer` (keep the clearer names for now).

### Stage 5: Document and merge

- [x] Update `docs/ARCHITECTURE.md`.
- [x] Update `docs/MIXER-CONTROL-API.md`.
- [x] Update `CLAUDE.md` architecture notes.
- [ ] Update `docs/CONFIG.md` if needed.
- [ ] Final merge `dev` → `main`.

## 10. Risks and open questions

| Risk | Mitigation |
|---|---|
| HRTF at 48 kHz then resampled to 44.1 kHz may shift spatial cues. | A/B listening test with `poc_hrtf`; if unacceptable, keep per-output spatializer for 44.1 kHz outputs as a fallback. |
| Group bus thread adds latency. | Keep group ring small (one render period); measure latency in `poc_wasapi`. |
| Input processor thread + multiple group consumers need a thread-safe `RingBuffer` reader. | `RingBuffer` is already single-producer/multi-consumer via `readOrSilence`; verify with multi-output test. |
| `InputProcessor` must support process loopback with same reliability as `Strip`. | Reuse `ProcessLoopbackActivationHandler` and `ActivateAudioInterfaceAsync` logic from `AudioMixer.cpp`. |
| RNNoise still requires 48 kHz; other source rates must be resampled first. | Resample before RNNoise; if CPU cost is too high, skip RNNoise for non-48 kHz captures as today. |
| Migration is large and hard to review. | Keep old and new code side-by-side with a runtime flag (`--legacy-matrix`) for one release cycle. |

## 11. Testing plan

- `tests/test_input_processor.cpp` (new): feed captured sine/sweep, verify DSP output matches current `poc_eq`/`poc_hrtf`/`poc_rnnoise` results.
- `tests/test_group_bus.cpp` (new): two inputs, one group, two outputs, verify gain/mute/send-gain math.
- `tests/test_output_mixer.cpp` (new): render to a WASAPI endpoint, verify no dropouts and correct level.
- `tests/test_mixer_live_edit.cpp` (existing): port to new backend and run add/rename/volume/remove cycle.
- Manual: start `route_cli mixer`, TUI, Loupedeck; run a real session with mic + music + Discord for 30 minutes.

## 12. Success criteria

- A mic with RNNoise + EQ + HRTF routed to three outputs runs **one** DSP chain, not three.
- CPU usage at equal channel counts is measurably lower.
- No functional regression: same state, same API, same TUI/ plugin behavior.
- No new allocations on the real-time audio thread after setup.
