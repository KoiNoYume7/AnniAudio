# AnniAudio — Per-Input DSP Refactor Design

**Status:** design / not implemented  
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
| `GroupBus` | One per group. Sums the `ProcessedInput` streams of its member inputs. Applies group volume, mute, and per-output send gains. Produces one interleaved stream per connected output. |
| `OutputMixer` | One per output. Sums group contributions already resampled to the output rate. Applies output master volume/mute and renders to WASAPI. |

### 5.2 Data flow

1. **Capture thread / input processor thread** wakes on capture event.
2. Read packets from `IAudioCaptureClient`.
3. Resample to a common processing rate if needed.
4. Run RNNoise (48 kHz) → EQ → optional HRTF spatialization.
5. Write processed stereo interleaved frames to `ProcessedInput::ring`.
6. **Render thread / output mixer thread** wakes on render event.
7. For each group connected to this output, read the group's contribution from `GroupBus::outputRing(output)`.
8. Sum, apply master, clamp, release WASAPI buffer.
9. **Group bus thread or inline in output thread:** a group can either be mixed inline by each output or maintain its own per-output rings. The design below picks per-output rings owned by the `GroupBus` and filled by a worker that wakes whenever any of its inputs has new data.

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
- Writes to a single-producer, single-consumer ring buffer consumed by group buses.
- Volume/mute/denoise/EQ/spatial/azimuth/elevation are atomic toggles read each callback.

### 7.2 Group bus mixing

Option A: **Inline in output mixer thread**
- Each `OutputMixer`, on its render callback, reads from every `GroupBus`'s input rings, applies send gains, and sums.
- No extra thread, but every output re-reads and re-sums the same group inputs.

Option B: **Dedicated group bus worker**
- `GroupBus` has its own thread that waits on a condition signaled by any of its input processors.
- It produces per-output rings at 48 kHz.
- Output mixers only read their pre-mixed group contribution.
- This is the preferred design because it centralizes group mixing and keeps `OutputMixer` cheap.

### 7.3 Output mixer thread

- One thread per `OutputMixer`.
- Waits on render event.
- Reads from group per-output rings, resamples to `renderRate`, sums, applies master, clamps, renders.

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

1. Implement `InputProcessor` class in `src/core/InputProcessor.{hpp,cpp}`.
   - Capture from device or application loopback.
   - Resample to 48 kHz.
   - Run denoise/EQ/spatial/colorEq.
   - Write to `RingBuffer` at 48 kHz stereo.
2. Implement `GroupBus` class in `src/core/GroupBus.{hpp,cpp}`.
   - Own a list of `ProcessedInput` rings.
   - Mix to per-output rings at 48 kHz.
   - Apply group volume/mute/send gains atomically.
3. Implement `OutputMixer` class in `src/core/OutputMixer.{hpp,cpp}`.
   - Resample from 48 kHz to `renderRate`.
   - Sum group contributions.
   - Apply output master and render.
4. Add unit/poc test: `tests/test_input_processor.cpp` feeds a sine wave, verifies denoise/EQ/spatial run once and the output is correct.

### Stage 2: New matrix backend

1. Create `AudioMixerMatrix2` or refactor `AudioMixerMatrix` behind a flag.
2. Map `InputConfig` → `InputProcessor`.
3. Map `GroupConfig` → `GroupBus`.
4. Map `OutputConfig` → `OutputMixer`.
5. Hook control API methods to the new backend.

### Stage 3: Feature parity + A/B testing

- Run `test_mixer_live_edit` against the new backend.
- Run the TUI and Loupedeck plugin against the new backend for a real session.
- Compare CPU usage with `route_cli` + Task Manager or a built-in `mixer` stats endpoint.

### Stage 4: Remove legacy `AudioMixer`/`Strip` path

- Delete `AudioMixer.cpp/hpp`, `AudioMixerMatrix` old route logic, and `AudioEngine.cpp` if it is still unused.
- Rename `OutputMixer` → `AudioMixer` if desired, or keep the clearer names.

### Stage 5: Document and merge

- Update `docs/ARCHITECTURE.md` and `docs/CONFIG.md`.
- Update `CLAUDE.md` architecture notes.
- Merge to `dev`, then `main`.

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
