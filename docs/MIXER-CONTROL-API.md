# AnniAudio — Mixer Control API

The mixer (`route_cli mixer`) exposes a local HTTP+SSE control API on `127.0.0.1`
(default port `8850`). Any client — the curses TUI, the Loupedeck plugin, a future
graphical UI, or a manual `curl` script — is just another consumer of this API.
This supersedes the earlier MIDI-CC-binding approach that lived directly inside
`cmdMixer`.

---

## Why not MIDI-in-the-mixer-config

The first pass wired MIDI CC/note messages directly into `route_cli mixer` (a
`"midi"` block in the mixer JSON, parsed and bound to mixer controls at startup).
It worked, but it was the wrong shape:

- It only worked while `route_cli mixer` itself was running with stdin attached —
  no GUI, no multi-client access, no way for a Logitech/Loupedeck **plugin** (which
  runs inside the Logi Plugin Service host process, not as a child of
  `route_cli`) to participate.
- MIDI CC numbers are not a stable, discoverable protocol — every user has to go
  run a separate "monitor raw MIDI" tool and hand-edit numbers into JSON.
- It coupled a specific input device's protocol (MIDI) to the mixer's core, when
  the mixer should not need to know anything about *what* controls it.

The correct shape: the mixer exposes a **generic control API**. Anything — a
graphical UI, a Loupedeck plugin, a future physical device, our own CLI — becomes
just another client of that API. `route_cli midi` remains as a small standalone
diagnostic (list MIDI devices, print raw messages) with no special relationship to
the mixer.

---

## Design Principles

Same principles as `docs/ARCHITECTURE.md`, applied to this layer specifically:

**API-first.** The TUI and the Loupedeck plugin are equally "just clients."
Nothing the TUI can do is unavailable to the API, and vice versa.

**Local-only, no auth.** The control server binds to `127.0.0.1` only and is never
exposed on any other interface. There is no authentication layer, because there is
nothing to authenticate against — this is a single-user, single-machine tool.
Binding to loopback only is the security boundary; do not relax this without
re-thinking the whole model.

**Never glitch audio for a control-plane operation.** Adjusting master/group volume
or mute is lock-free and glitch-free (`std::atomic<float>`/`std::atomic<bool>`,
read directly by the audio thread). Adding, removing, renaming, or re-sourcing an
input or group is a *structural* change handled on the control thread; it never
causes a dropout on any *other* input's audio or on the render output.

**IDs, not positions.** A group's index in a list is not a safe identifier once
groups can be added or removed while running. Every input and group gets a stable,
opaque, monotonically increasing `id` at creation time. All control-API and public
`AudioMixerMatrix` calls address inputs and groups by `id`.

---

## Threading model: seamless structural changes

The audio graph is decoupled by ring buffers:

- Each `InputProcessor` writes its processed 48 kHz stereo signal to a
  `MultiReaderRingBuffer`.
- Each `GroupBus` (one per group+output pair) reads from each input's ring with its
  own cursor; `readOrSilence()` never blocks and silence-pads if an input is
  momentarily behind.
- Each `OutputMixer` render thread calls `GroupBus::mix()` into a scratch buffer,
  sums the results, applies master volume/mute, and writes to the render device.

This means the render output is already immune to hiccups on any individual input's
capture side. Structural changes build on top of that guarantee.

**Control thread.** All mutating `AudioMixerMatrix` operations (`addOutput`,
`addInput`, `addGroup`, `setGroupInputIds`, `setGroupOutputIds`, `updateInput`,
`removeInput`, etc.) run on the HTTP/CLI thread under one `std::mutex` and call
`rebuildLocked()`. `rebuildLocked()`:

1. Builds the desired set of `(group, output)` buses from the current `groups` and
   `outputs` maps.
2. Removes any `GroupBus` no longer desired and detaches it from its `OutputMixer`.
3. Creates missing `GroupBus` instances and attaches them to the corresponding
   `OutputMixer`.
4. For each bus, ensures each configured input has a running `InputProcessor`,
   adding or removing inputs from the bus as needed.
5. Prunes `InputProcessor` instances that are no longer referenced by any bus.
6. Stops `OutputMixer` instances with no buses and starts ones that have buses.

The audio paths are reconfigured between render callbacks; a running `OutputMixer`
snapshots its bus list under a short lock at the start of each `processRender()`
call, and each `GroupBus` snapshots its source list under a short lock at the start
of each `mix()` call. Shared pointers keep buses alive if they are removed while a
render pass is still using them.

Volume, mute, and HRTF direction changes are atomic and do not require a rebuild:
- `InputProcessor` `setDirection()` stores the requested azimuth/elevation atomically;
  the capture thread applies it before the next processed packet.
- `GroupBus` `setGain()` / `setMuted()` update atomics read directly in `mix()`.
- `OutputMixer` `setMasterVolume()` / `setMasterMuted()` update atomics read directly
  in `processRender()`.

Because structural edits happen at human time scales (a handful of times per
session), the short mutex held during `rebuildLocked()` is not a real-time risk. A
lock-free queue for this frequency would be over-engineering that adds bug surface
for no real benefit.

---

## Public API

`AudioMixerMatrix` (`src/core/AudioMixerMatrix.hpp`) is the routing matrix. It owns
`InputProcessor` capture/DSP instances (one per input), `GroupBus` mix instances
(one per group+output pair), and `OutputMixer` render instances (one per output).
It exposes the matrix-level identifiers `InputId` and `GroupId`.

```cpp
using InputId  = uint32_t;
using GroupId  = uint32_t;

struct InputConfig {
    std::string name;
    std::string type = "device";  // "device" or "application"
    bool        denoise = false;   // RNNoise (48 kHz only)
    std::string eqPreset;          // "" = off; "voice" = built-in voice EQ
    bool        spatial = false;   // HRTF binaural positioning
    float       azimuth = 0.0f;    // 0 = front, +90 = left, -90 = right
    float       elevation = 0.0f;  // 0 = ear level, +90 = above
    std::string source;            // endpoint name or decimal PID
};

struct GroupConfig {
    std::string name;
    std::string color = "#3b82f6";
    std::string cable;             // optional render VAC for per-app routing
    std::vector<InputId>  inputIds;
    std::vector<std::string> outputIds;
    std::map<std::string, float> outputGains; // output name -> send gain, 1.0 = unity
    float       volume = 1.0f;
    bool        muted  = false;
    std::optional<int> knobIndex;
};

struct InputSnapshot {
    InputId id = 0;
    std::string name;
    std::string type;
    std::string source;
    bool denoise = false;
    std::string eqPreset;
    bool spatial = false;
    float azimuth = 0.0f;
    float elevation = 0.0f;
    float peak = 0.0f;
    float rms = 0.0f;
};

struct GroupSnapshot {
    GroupId id = 0;
    std::string name;
    std::string color;
    std::string cable;
    std::vector<InputId> inputIds;
    std::vector<std::string> outputIds;
    std::map<std::string, float> outputGains; // percent
    float volume = 100.0f;
    bool muted = false;
    std::optional<int> knobIndex;
    float peak = 0.0f;
    float rms = 0.0f;
};

struct OutputSnapshot {
    std::string name;
    float master = 100.0f;
    bool muted = false;
    std::vector<GroupId> groupIds;
    float masterPeak = 0.0f;
    float masterRms = 0.0f;
};

struct MixerStateSnapshot {
    bool running = false;
    uint16_t controlPort = 8850;
    std::vector<InputSnapshot>  inputs;
    std::vector<GroupSnapshot>  groups;
    std::vector<OutputSnapshot> outputs;
};

class AudioMixerMatrix {
public:
    // Outputs
    bool addOutput(const std::string& outputHint);
    bool removeOutput(const std::string& outputName);
    std::vector<std::string> outputNames() const;
    float outputMasterVolume(const std::string& outputName) const;
    float outputMasterPeak(const std::string& outputName) const;
    float outputMasterRms(const std::string& outputName) const;
    void  setOutputMasterVolume(const std::string& outputName, float v);
    bool  setOutputMuted(const std::string& outputName, bool muted);
    bool  outputMuted(const std::string& outputName) const;

    // Inputs
    std::optional<InputId> addInput(const InputConfig& cfg);
    bool removeInput(InputId id);
    bool updateInput(InputId id, const InputConfig& cfg);
    bool setInputDirection(InputId id, float azimuth, float elevation);

    // Groups
    std::optional<GroupId> addGroup(const GroupConfig& cfg);
    bool removeGroup(GroupId id);
    bool setGroupName(GroupId id, const std::string& name);
    bool setGroupColor(GroupId id, const std::string& color);
    bool setGroupCable(GroupId id, const std::string& cable);
    bool setGroupVolume(GroupId id, float vol);
    bool setGroupMuted(GroupId id, bool muted);
    bool setGroupOutputGain(GroupId id, const std::string& output, float gainPct);
    bool setGroupKnobIndex(GroupId id, std::optional<int> knobIndex);
    bool setGroupInputIds(GroupId id, std::vector<InputId> ids);
    bool setGroupOutputIds(GroupId id, std::vector<std::string> ids);
    bool addGroupInput(GroupId groupId, InputId inputId);
    bool removeGroupInput(GroupId groupId, InputId inputId);
    bool addGroupOutput(GroupId groupId, const std::string& outputName);
    bool removeGroupOutput(GroupId groupId, const std::string& outputName);

    // Transport / info
    bool start();
    void stop();
    bool running() const;
    MixerStateSnapshot snapshot() const;
    std::vector<EndpointInfo> listEndpoints() const;
    std::vector<ApplicationInfo> listApplications() const;

    // Persistence
    bool save(const std::string& path) const;
    bool load(const std::string& path);
    void setAutosavePath(const std::string& path);
    void setControlPort(uint16_t port);
    bool autosaveEnabled() const;
};
```

`route_cli mixer`'s interactive text commands (`o <n>`, `+`, `-`, `v <n> <vol>`,
`m <n>`) still address groups and outputs by their **1-based position in the
current listing** for human convenience at a terminal.

---

## Control API surface

Embedded HTTP server inside the `route_cli mixer` process, bound to
`127.0.0.1:<port>` (default port **8850**, configurable via a `--port`/`-p` CLI
flag or a `controlPort` field in the mixer JSON). A `controlPort` value of `0`
disables the control API entirely. Built on **cpp-httplib** (single-header,
MIT-licensed), vendored under `third_party/httplib/`.

All bodies are JSON. Errors are `{ "error": "human-readable message" }` with a
4xx/5xx status.

| Method   | Path                    | Body                                           | Description |
|----------|-------------------------|-------------------------------------------------|-------------|
| `GET`    | `/api/state`            | —                                               | Full snapshot: `{ running, controlPort, inputs, groups, outputs }` |
| `GET`    | `/api/endpoints`        | —                                               | Live WASAPI endpoints, for the source picker |
| `GET`    | `/api/applications`     | —                                               | Running audio sessions (process id, `name`, `displayName`, `windowTitle`, `endpoint`, `isInput`, `isActive`, `isMuted`, `volume`, `isSystem`). One entry per process; the reported endpoint is the session actually playing (active preferred over inactive, render over capture). `windowTitle` is the app's main window title (resolved through same-exe parent processes for windowless audio children), for identification only |
| `GET`    | `/api/events`           | —                                               | Server-Sent Events stream; pushes a `state` event on every change, plus a periodic heartbeat comment |
| `POST`   | `/api/inputs`           | `{ name, type, source, denoise?, eqPreset?, spatial?, azimuth?, elevation? }` | Add an input source (type = `device` or `application`). `denoise` enables RNNoise suppression (48 kHz captures); `eqPreset` = `"voice"` enables the built-in voice EQ. `spatial` enables HRTF binaural positioning at `azimuth`/`elevation` (degrees; azimuth 0 = front, +90 = left, -90 = right; elevation 0 = ear level, +90 = above) — requires a stereo-or-wider output. All run engine-side before mixing |
| `PATCH`  | `/api/inputs/{id}`      | `{ name?, type?, source?, denoise?, eqPreset?, spatial?, azimuth?, elevation? }`| Partial update of an input (omitted fields keep their value); re-creates routes for any groups that use it. Use this to toggle `spatial` on/off; for smooth live direction changes prefer `POST /api/inputs/{id}/direction` |
| `POST`   | `/api/inputs/{id}/direction` | `{ azimuth?, elevation? }`                 | Live HRTF direction change for a spatialized input. Updates the running spatializer in place without rebuilding its routes — glitch-free and safe at knob-turn rates. No-op on the audio if the input isn't spatial (value is still stored) |
| `DELETE` | `/api/inputs/{id}`      | —                                               | Remove an input and remove it from all groups |
| `POST`   | `/api/groups`           | `{ name, color?, cable?, inputIds?, outputIds?, volume?, muted?, knobIndex? }` | Add a group (mix bus). Set per-output send gains with `PATCH /api/groups/{id}` after creation |
| `PATCH`  | `/api/groups/{id}`      | `{ name?, color?, cable?, inputIds?, outputIds?, outputGains?, volume?, muted?, knobIndex? }` | Partial update of a group. `outputGains` is `{ "<output name>": percent }` — the group's send level towards that output (100 = unity); effective route volume is group volume × send gain |
| `DELETE` | `/api/groups/{id}`      | —                                               | Remove a group |
| `POST`   | `/api/outputs`          | `{ name }`                                      | Add a render output by endpoint name |
| `DELETE` | `/api/outputs`          | `{ name }`                                      | Remove an output and disconnect every group from it |
| `POST`   | `/api/outputs/master`   | `{ name, volume?, muted? }`                     | Set an output's master volume and/or mute (at least one of `volume`/`muted` required) |
| `GET`    | `/api/scenes`           | —                                               | List saved scenes (`config/scenes/*.json`) |
| `POST`   | `/api/scenes/save`      | `{ name }`                                      | Snapshot current levels (group volumes/mutes/send gains, output masters/mutes) as a named scene |
| `POST`   | `/api/scenes/apply`     | `{ name }`                                      | Apply a scene by matching group/output NAMES; instant level overlay, never a topology change. Returns `{ applied, missing }` |
| `POST`   | `/api/presets/save`     | `{ path? }`                                     | Write current live state to a `config/mixers/*.json` preset file; empty `path` uses the configured autosave path |

**State model: inputs → groups → outputs.**
- An *input* is a source (`device` = WASAPI endpoint, `application` = process ID captured via process loopback).
- A *group* is a mix bus: one fader/mute, a color, an optional `cable` (a render endpoint used for per-app routing), a list of input IDs, and a list of output names it feeds.
- An *output* is a render endpoint with a master fader.

The optional `cable` field is the VAC/render endpoint the TUI routes an application to when the user adds an `application` input to the group. The mixer then captures that same-named capture endpoint as a `device` input, which avoids double audio because the application's original output is redirected into the cable.

If a group has no `cable`, adding an `application` input falls back to process loopback capture. That works without a VAC but the application will still play on its original device, causing double audio.

**Optional `knobIndex` field.** Each group may carry an optional `knobIndex: 0..5 | null` in its config/state, settable from the TUI or any API client. This gives the Loupedeck plugin (or any future physical controller) a stable mapping from a physical control to a specific group that survives groups being reordered, added, or removed.

**SSE, not polling.** `/api/events` is the source of truth for "did anything change" for both the TUI and the Loupedeck plugin. Every structural or volume/mute change broadcasts a `state` event to all connected listeners.

**COM/WASAPI threading note.** HTTP worker threads that touch `IMMDeviceEnumerator`/`IMMDevice` (e.g. `/api/endpoints`, or preparing a new input for `POST /api/inputs`) must call `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` once per thread before doing so, matching the apartment model already used by `InputProcessor` and `OutputMixer` audio threads. cpp-httplib's worker threads are plain OS threads with no COM initialization by default.

---

## TUI

`scripts/mixer_tui.py` is a curses terminal interface. It runs three threads: the
curses render loop, an API worker that drains queued PATCH/POST/DELETE calls so
slow requests never freeze the UI, and an SSE worker holding `/api/events` (with
reconnect-on-drop) so the view stays in sync with any other client.

Run `start-mixer.bat` to launch `route_cli mixer`, then `mixer-tui.bat` to start
the TUI. See `docs/TUI.md` for the complete keybinding reference and the
`--repair-routes` flag.

---

## Loupedeck Live plugin

`AnniAudioMixerPlugin/` is a C# Logi Actions SDK plugin and a pure HTTP client of
the control API. It is built with:

```powershell
dotnet build AnniAudioMixerPlugin/src/AnniAudioMixerPlugin.csproj -c Release
```

Actions mirror live mixer state and update automatically when groups/outputs/scenes
are renamed, added, or removed. The post-build step links the plugin into the Logi
Plugin Service and hot-reloads it. See `AnniAudioMixerPlugin/README.md` for details.

---

## Migration notes

- Removed: the `"midi"` block in `config/mixers/*.json` and its handling in
  `cmdMixer`. Mixer JSON files are now pure presets. The current shape is
  `inputs`, `groups`, and `outputs`. Legacy shapes (`output` + `strips`, or
  `outputs` + `routes`) may still be accepted and converted automatically on load.
- Kept: `route_cli midi list` / `route_cli midi <device-hint>` as a standalone
  diagnostic with no relationship to the mixer.
- `route_cli mixer` drives `AudioMixerMatrix`, which owns `InputProcessor`
  (one per input), `GroupBus` (one per group+output pair), and `OutputMixer` (one
  per output). The interactive text commands resolve position → group/output at the
  point of use.
