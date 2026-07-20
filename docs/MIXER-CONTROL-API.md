# AnniAudio — Mixer Control API & GUI Design

Design for turning `route_cli mixer` into a properly controllable service: a local HTTP+SSE
control API embedded in the mixer process, a dedicated web GUI on top of it, and (later) a real
Loupedeck Live plugin that talks to the same API. This supersedes the earlier MIDI-CC-binding
approach that lived directly inside `cmdMixer` (see "Migration notes" at the end).

---

## Why not MIDI-in-the-mixer-config

The first pass wired MIDI CC/note messages directly into `route_cli mixer` (a `"midi"` block in
the mixer JSON, parsed and bound to strips at startup). It worked, but it was the wrong shape:

- It only worked while `route_cli mixer` itself was running with stdin attached — no GUI, no
  multi-client access, no way for a Logitech/Loupedeck **plugin** (which runs inside the Logi
  Plugin Service host process, not as a child of `route_cli`) to participate.
- MIDI CC numbers are not a stable, discoverable protocol — every user has to go run a separate
  "monitor raw MIDI" tool and hand-edit numbers into JSON.
- It coupled a specific input device's protocol (MIDI) to the mixer's core, when the mixer
  should not need to know anything about *what* controls it.

The correct shape: the mixer exposes a **generic control API**. Anything — a browser GUI, a
Loupedeck plugin, a future physical device, our own CLI — becomes just another client of that
API. `route_cli midi` remains as a small standalone diagnostic (list MIDI devices, print raw
messages) with no special relationship to the mixer.

---

## Design Principles

Same principles as `docs/ARCHITECTURE.md`, applied to this layer specifically:

**API-first.** The GUI and the Loupedeck plugin are equally "just clients." Nothing the GUI can
do is unavailable to the API, and vice versa.

**Local-only, no auth.** The control server binds to `127.0.0.1` only and is never exposed on
any other interface. There is no authentication layer, because there is nothing to authenticate
against — this is a single-user, single-machine tool. Binding to loopback only is the security
boundary; do not relax this without re-thinking the whole model.

**Never glitch audio for a control-plane operation.** Adjusting master/strip volume or mute is
already lock-free and glitch-free today (`std::atomic<float>`/`std::atomic<bool>`, read directly
by the audio thread). Adding, removing, renaming, or re-sourcing a strip is a *structural* change
and needs new machinery — but it must still never cause a dropout on any *other* strip's audio or
on the render output. See "Threading model" below for how this is guaranteed.

**IDs, not positions.** Once strips can be added/removed while running, a strip's index in a list
is not a safe identifier — a GUI mid-drag on "strip 3" must not suddenly be controlling a
different strip because "strip 2" was deleted a moment earlier. Every strip gets a stable,
opaque, monotonically increasing `id` at creation time. All control-API and public `AudioMixer`
calls address strips by `id`.

---

## Threading model: seamless structural changes

The render path is already decoupled from each strip's capture path by a per-strip
`RingBuffer` (see `src/core/audio_utils.hpp`): `processRender()` only ever reads from ring
buffers via `readOrSilence()`, which never blocks — it silence-pads if a strip is momentarily
behind. This means the render output is already immune to hiccups on any individual strip's
capture side. Structural strip changes build on top of that guarantee rather than needing a new
one.

**Command queue.** Add/remove/rename/re-source strip operations are represented as `Command`
values pushed onto a small, mutex-protected queue:

```cpp
struct Command {
    enum class Kind { AddStrip, RemoveStrip, RenameStrip } kind;
    StripId               id;            // target strip (Remove, Rename); new id (Add)
    std::unique_ptr<Strip> preparedStrip; // Add only — see below
    std::string           text;          // Rename only — new name
};
```

The **caller thread** (an HTTP request handler) does all the work that can fail or block *before*
touching the queue:

- `AddStrip`: resolve the source endpoint, activate the `IAudioClient`, negotiate format,
  allocate buffers — i.e. everything `Impl::openStrip()` already does today. If any of this
  fails, return an error to the HTTP client immediately; nothing is queued and the audio thread
  is never involved. Only a fully-prepared, ready-to-run `Strip` is handed to the queue.
- `RemoveStrip` / `RenameStrip`: these can't fail in a way that matters (unknown id → HTTP 404
  before queueing), so there's nothing to prepare.

The **audio thread**, at the top of every iteration of its main loop (i.e. up to a couple hundred
times a second, immediately before the blocking `WaitForMultipleObjects` call), does a quick
`std::lock_guard` + `std::vector::swap` to grab any pending commands, then applies them:

- `AddStrip`: `strips.push_back(std::move(cmd.preparedStrip))`, calls `IAudioClient::Start()` on
  it, marks the handle array dirty.
- `RemoveStrip`: calls `IAudioClient::Stop()` and closes the strip's event handle, erases it from
  `strips`, marks the handle array dirty.
- `RenameStrip`: just updates the `std::string name` — no thread-safety concerns beyond the same
  lock, since nothing else touches that field.

If the handle array is marked dirty, it's rebuilt from `strips` before the next
`WaitForMultipleObjects` call. This is a handful of pointer copies — negligible next to the
200 ms max wait timeout already in the loop.

This queue is a plain mutex, not a lock-free SPSC/MPSC structure. That is a deliberate choice:
lock-free queues earn their complexity when messages are per-sample or otherwise very
high-frequency. Strip edits happen a handful of times per session, driven by a human clicking
"add strip" in a GUI. A mutex held for a `vector::swap` is not a professional risk here; a
hand-rolled lock-free queue for this frequency would be over-engineering that adds bug surface
for no real benefit.

**Net effect:** every *other* strip's capture → ring buffer → render chain is completely
unaffected by an add/remove/rename happening concurrently. The strip being removed obviously
stops (that's the point), and the operation itself completes within one audio-thread loop
iteration (≤ tens of milliseconds).

---

## Public API changes

`AudioMixer`'s current index-based methods (`setStripVolume(size_t idx, ...)`,
`stripVolume(size_t idx)`, etc.) are unsafe once strips can be removed at runtime and get
renumbered under a caller's feet. They will be replaced with an opaque `StripId`:

```cpp
using InputId = uint32_t;
using GroupId = uint32_t;

struct InputConfig  { std::string name, type, source; };
struct GroupConfig  { std::string name, color, cable; std::vector<InputId> inputIds; std::vector<std::string> outputIds; float volume; bool muted; std::optional<int> knobIndex; };

struct MixerStateSnapshot {
    bool running;
    uint16_t controlPort;
    std::vector<InputSnapshot>  inputs;
    std::vector<GroupSnapshot>  groups;
    std::vector<OutputSnapshot> outputs;
};

class AudioMixerMatrix {
public:
    bool addOutput(const std::string& outputHint);
    bool removeOutput(const std::string& outputName);
    void setOutputMasterVolume(const std::string& outputName, float v); // v in percent

    std::optional<InputId> addInput(const InputConfig& cfg);
    bool removeInput(InputId id);
    bool updateInput(InputId id, const InputConfig& cfg);

    std::optional<GroupId> addGroup(const GroupConfig& cfg);
    bool removeGroup(GroupId id);
    bool setGroupName(GroupId id, const std::string& name);
    bool setGroupColor(GroupId id, const std::string& color);
    bool setGroupVolume(GroupId id, float v); // v in percent
    bool setGroupMuted(GroupId id, bool muted);
    bool setGroupInputIds(GroupId id, std::vector<InputId> ids);
    bool setGroupOutputIds(GroupId id, std::vector<std::string> ids);

    MixerStateSnapshot snapshot() const;
    std::vector<EndpointInfo> listEndpoints() const;

    bool save(const std::string& path) const;
    bool load(const std::string& path);
    void setAutosavePath(const std::string& path);
};
```

`route_cli mixer`'s interactive text commands (`v <n> <vol>`, `m <n>`, `o <n>`, `+`, `-`) still address groups and outputs by their **1-based position in the current listing** for human convenience at a terminal.

---

## Control API surface

Embedded HTTP server inside the `route_cli mixer` process, bound to `127.0.0.1:<port>` (default
port **8850**, configurable via a `--port`/`-p` CLI flag or a `controlPort` field in the mixer
JSON). A `controlPort` value of `0` disables the control API entirely. Built on **cpp-httplib**
(single-header, MIT-licensed) — vendored under `third_party/httplib/`, same trust tier as the
already-vendored `nlohmann/json`.

All bodies are JSON. Errors are `{ "error": "human-readable message" }` with a 4xx/5xx status.

| Method   | Path                    | Body                                           | Description |
|----------|-------------------------|-------------------------------------------------|-------------|
| `GET`    | `/api/state`            | —                                               | Full snapshot: `{ running, controlPort, inputs, groups, outputs }` |
| `GET`    | `/api/endpoints`        | —                                               | Live WASAPI endpoints, for the source picker |
| `GET`    | `/api/applications`     | —                                               | Running audio sessions (process id, name, endpoint, mute, volume, `isActive`, `windowTitle`). One entry per process; the reported endpoint is the session actually playing (active preferred over inactive, render over capture). `windowTitle` is the app's main window title (resolved through same-exe parent processes for windowless audio children), for identification only |
| `GET`    | `/api/events`           | —                                               | Server-Sent Events stream; pushes a `state` event on every change, plus a periodic heartbeat comment |
| `POST`   | `/api/inputs`           | `{ name, type, source, denoise?, eqPreset? }`   | Add an input source (type = `device` or `application`). `denoise` enables RNNoise suppression (48 kHz captures); `eqPreset` = `"voice"` enables the built-in voice EQ. Both run engine-side before mixing |
| `PATCH`  | `/api/inputs/{id}`      | `{ name?, type?, source?, denoise?, eqPreset? }`| Partial update of an input (omitted fields keep their value); re-creates routes for any groups that use it |
| `DELETE` | `/api/inputs/{id}`      | —                                               | Remove an input and remove it from all groups |
| `POST`   | `/api/groups`           | `{ name, color?, cable?, inputIds?, outputIds?, volume?, muted?, knobIndex? }` | Add a group (mix bus) |
| `PATCH`  | `/api/groups/{id}`      | `{ name?, color?, cable?, inputIds?, outputIds?, outputGains?, volume?, muted?, knobIndex? }` | Partial update of a group. `outputGains` is `{ "<output name>": percent }` — the group's send level towards that output (100 = unity); effective route volume is group volume x send gain |
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

**Optional `knobIndex` field.** Each group may carry an optional `knobIndex: 0..5 | null` in its
config/state, settable from the GUI ("assign to knob N"). This gives the Loupedeck plugin (or any
future physical controller) a stable mapping from a physical control to a specific group that
survives groups being reordered, added, or removed.

**SSE, not polling.** `/api/events` is the source of truth for "did anything change" for both the
GUI and the future plugin. Every structural or volume/mute change broadcasts a `state` event to
all connected listeners.

**COM/WASAPI threading note.** HTTP worker threads that touch `IMMDeviceEnumerator`/`IMMDevice`
(e.g. `/api/endpoints`, or preparing a new strip for `POST /api/strips`) must call
`CoInitializeEx(nullptr, COINIT_MULTITHREADED)` once per thread before doing so, matching the
apartment model already used by `AudioEngine`/`AudioMixer`'s own audio thread. cpp-httplib's
worker threads are plain OS threads with no COM initialization by default.

---

## TUI

`scripts/mixer_tui.py` is a curses terminal interface:

- **Virtual Cables pane** on the left: one header row per virtual cable, color-coded, with its fader,
  meter, mute indicator and connected outputs. Virtual cables are collapsed by default; expand one
  to see its applications/sources.
- **Outputs pane** on the right: one row per render output, with a master fader, meter, and the
  list of virtual cables feeding it.
- Keyboard controls: `Tab` switches panes, `j`/`k` navigate, `+`/`-`/`v` adjust volume, `m` mutes a
  virtual cable, `Enter`/`e` expands/collapses a virtual cable's application list, `i` adds a
  device source to the selected virtual cable, `a` adds a running application to the selected virtual
  cable, `g` adds a virtual cable, `o` adds an output, `c` connects/disconnects the selected virtual
  cable to/from the selected output, `r` renames a virtual cable, `d` deletes the selected virtual
  cable/application/output, `s` saves the preset, `R` refreshes the endpoint and application lists.
- Runs three threads: the curses render loop, a worker that drains queued PATCH/POST/DELETE
  calls so slow requests never freeze the UI, and one holding the `/api/events` SSE connection
  (with reconnect-on-drop) so the view stays in sync with any other client.

Run `start-mixer.bat` to launch `route_cli mixer`, then `mixer-tui.bat` to start the TUI.

(The web GUI at `scripts/mixer.html` is currently outdated and reflects the older strips/routes API.)

---

## Loupedeck Live plugin (future work, not built yet)

Once the control API exists, the plugin (C# via the Logi Actions SDK, scaffolded under
`AnniAudioMixerPlugin/`) is a pure HTTP client:

- On load, `GET /api/state` to find groups with a `knobIndex`, and subscribes to `/api/events`
  to stay in sync (e.g. if a group is removed from the GUI while the plugin is running).
- On knob rotation, `PATCH /api/groups/{id}` with the new volume for whichever group has that
  `knobIndex`.
- On knob press, `PATCH /api/groups/{id}` with `{ "muted": !current }`, using its locally
  cached state (updated via SSE) rather than a `GET` round-trip per press.
- Displays the group name/level on the Loupedeck's own screen via the SDK's
  `PluginDynamicAdjustment`/`GetAdjustmentValue`.

---

## Migration notes

- Removed: the `"midi"` block in `config/mixers/*.json` and its handling in `cmdMixer`.
  Mixer JSON files are now pure presets. The new preferred shape is `inputs`, `groups`, and `outputs`.
  Legacy shapes (`output` + `strips`, or `outputs` + `routes`) are still accepted and converted
  automatically on load.
- Kept: `route_cli midi list` / `route_cli midi <device-hint>` as a standalone diagnostic with no
  relationship to the mixer.
- `AudioMixer`'s `size_t`-indexed strip API has been replaced by a `StripId`-based one, and
  `route_cli mixer` now drives an `AudioMixerMatrix` that owns one `AudioMixer` per output.
  The interactive text commands resolve position → group/output at the point of use.
