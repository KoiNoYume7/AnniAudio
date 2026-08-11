# CLAUDE.md

Operating manual for AnniAudio. Read this before changing anything.

## What this is

A system-wide audio suite for Windows 11. The part under active development is the
**mixer**: a live routing matrix (inputs → groups/cables → outputs) with per-app
routing, DSP (RNNoise + EQ + HRTF spatialization), per-output send gains, scenes,
and a REST+SSE control API. Three clients drive that API: a Python curses TUI, a Loupedeck C# plugin, and
any HTTP client. Version: see `VERSION` (0.2.0). Default branch of work: `dev`.

## Architecture (mixer)

Audio flows one way: **inputs → groups → outputs**.

- `src/core/InputProcessor.cpp` (`audio_core` lib) — one instance **per input**.
  Captures a device or application loopback, runs the capture-side DSP chain
  (`NoiseSuppressor`, `EqChain`, optional HRTF spatialization), and writes the
  processed 48 kHz stereo interleaved result to a `MultiReaderRingBuffer`.
- `src/core/GroupBus.cpp` — one instance per **(group, output)** pair. Mixes the
  processed inputs of a group, applies the group volume/mute, and resamples to the
  output's sample rate and channel count. It is driven from the `OutputMixer` render
  callback, so each output has its own read cursor and send gain.
- `src/core/OutputMixer.cpp` — one instance **per output**. WASAPI render endpoint;
  sums the `GroupBus` signals feeding it, applies master volume/mute, and writes to
  the device. It renders at a fixed 48 kHz float format and relies on
  `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` for device format conversion.
- `src/core/AudioMixerMatrix.cpp` — orchestrates the above. Holds `inputs`,
  `groups`, `outputs`, and the `groupBuses` map. A group's effective volume into an
  output = `group.volume × outputGains[output]`. Structural changes are guarded by
  one `mtx`; `maybeAutosave()` writes after every mutation.
- `src/core/MixerControlServer.cpp` — httplib HTTP + SSE server on `127.0.0.1:8850`.
  SSE broadcasts full state on any `markDirty()`. Full endpoint list in
  `docs/MIXER-CONTROL-API.md` — **keep that doc in sync when you touch routes**.
- `src/dsp/` (`audio_dsp` lib) — `EqChain` (biquads), `NoiseSuppressor` (RNNoise),
  and `Spatializer` (HRTF FFT overlap-add convolution). Real-time-safe after
  `prepare()`.
- `src/core/AudioEngine.cpp` — the older single source→output engine, used only by
  `route_cli process`. Not part of the mixer path.
- `cli/anniaudio.ps1` — PowerShell driver/signing control panel (`install`, `uninstall`,
  `dev-mode`, `gaming-mode`, `config`, `status`, `build`, `route`, `tui`). The `tui`
  subcommand launches `mixer-tui.bat`.
- `src/cli/anniaudio-cli.cpp` — Phase 4 standalone CLI client for the mixer API
  (built as `anniaudio-cli.exe`).
- `tests/` — Phase-0 POCs and verification tools. Not built by default; use
  `-DBUILD_TESTS=ON`. `test_mixer_matrix` loads and runs the new `AudioMixerMatrix`
  pipeline end-to-end; `test_new_pipeline` tests `InputProcessor → GroupBus →
  OutputMixer` in isolation. `test_mixer_live_edit` has been removed along with the
  legacy `AudioMixer`/`Strip` path.
- `scripts/mixer_tui.py`, `scripts/tui_utils.py`, `scripts/tui_ui.py` — curses TUI
  split into entry/utility/ui modules.

Data model note: DSP is a property of an **input** (`denoise`, `eqPreset`, `spatial`,
`azimuth`, `elevation`), so the processed signal follows a mic through every route.
Send gain is a property of a **group→output** pair. Mute exists on groups and on
outputs. See `docs/CONFIG.md` for the config file reference.

## Build

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64      # once
cmake --build build --target route_cli --config Release
```

- **The running mixer locks `build/bin/Release/route_cli.exe`.** Builds compile fine
  but fail at link with `LNK1104: cannot open file ... route_cli.exe`. This is
  expected, not a code error — it means compilation succeeded. To actually relink,
  stop the mixer first: `.\stop-mixer.bat` (autosave makes the force-kill lossless),
  or check `Get-Process route_cli`. Don't kill it blindly if the user is mid-session;
  a clean compile is enough to verify C++ changes.
- Loupedeck plugin: `dotnet build AnniAudioMixerPlugin/src/AnniAudioMixerPlugin.csproj
  -c Release`. Post-build links it into the Logi Plugin Service and hot-reloads it.
  Needs `PluginApi.dll` from `C:\Program Files\Logi\LogiPluginService\` (net8.0).
  Verify load via `%LOCALAPPDATA%\Logi\LogiPluginService\Logs\plugin_logs\AnniAudioMixer.log`.

## Run / verify

```powershell
.\start-mixer.bat            # mixer + control API on 8850 (loads config/mixers/main.json)
.\mixer-tui.bat              # curses TUI client (separate terminal)
.\anniaudio-cli.exe state    # standalone CLI: state, groups, outputs, set-volume, mute, etc.
.\stop-mixer.bat             # stop it (before rebuilding)
.\install-autostart.bat      # hidden-at-logon launcher via Task Scheduler task (user session, not a service; no admin). `uninstall` arg removes it
```

The mixer is usually already running on 8850 with the user's real audio. You can
inspect live state read-only with `curl -s http://127.0.0.1:8850/api/state` and
`/api/applications`, `/api/endpoints`, `/api/scenes`. Prefer this over guessing.

## Electron GUI (Phase 5)

The graphical mixer lives under `gui/` and is a vanilla-JS Electron app.

```powershell
cd gui
npm install              # once
npm start                # dev: launches Electron against 127.0.0.1:8850
.\..\mixer-gui.bat      # or use the repo root launcher
```

- `main.cjs` is the Electron main process; `src/app.js` is the renderer entry.
- The renderer polls `GET /api/state` every 500ms. The WebSocket endpoint
  (`/api/ws`) sends an initial snapshot but the current `httplib` WebSocket
  handler can block the broadcast thread, so the GUI uses reliable HTTP polling
  for now.
- Some shells set `ELECTRON_RUN_AS_NODE=1`, which breaks `app.whenReady()`.
  `mixer-gui.bat` and `package.json` now clear that variable before launching.

## Checks before committing

- TUI (Python): `python -m pyflakes scripts/mixer_tui.py scripts/tui_utils.py scripts/tui_ui.py && python -m py_compile scripts/mixer_tui.py scripts/tui_utils.py scripts/tui_ui.py`
- C++: a clean *compile* (link may fail on the exe lock — that's fine).
- Plugin: `dotnet build ... -c Release` (0 warnings/errors).

## Conventions

- **Commit messages** end with the Co-Authored-By trailer. Work on `dev`, not `main`.
- Line endings are CRLF on Windows; git warns on LF→CRLF — harmless, ignore.
- TUI threading: the curses render loop only *reads* shared state under `state_lock`;
  never do blocking HTTP/COM on it. Slow actions go through `run_job()` (job worker);
  fire-and-forget mutations through `api_call()` (api worker). Optimistic values are
  held briefly so the UI doesn't rubber-band against the ~200ms SSE echo.
- Plugin: actions never touch audio; they call the control API through `MixerClient`,
  which polls state, coalesces writes, and holds locally-written values against stale
  polls. Action parameters are re-synced from live state so the device follows
  renames/adds/removes.

## Windows audio platform truths (these caused real bugs)

- **The default render device is a catch-all.** The user's default is the "System"
  VAC, so any un-routed app is still audible through the System group. New apps are
  never silent — auto-assignment only *moves* them, it doesn't rescue them.
- **Per-app routing only takes effect when the app recreates its audio stream.** A
  running app keeps playing on its old device until restarted; the config can be
  correct while the audio hasn't moved. The TUI shows a route-mismatch warning for
  this (only after the app is *actively* playing on the wrong endpoint for a grace
  period — transient startup sessions must not trigger it).
- **Elevated processes cannot be routed** by `winappaudiorouter` (e.g. rustdesk.exe →
  `E_INVALIDARG`/`0x80070057`). The route-repair loop parks a pid after repeated
  failures and drops dead pids, so it must never spam. Don't reintroduce blind retry.
- **RNNoise requires 48 kHz** capture (its training rate). Other rates are skipped
  with a log line, not resampled (yet).
- Process/session enumeration: one entry per pid, preferring the *actively playing*
  session; exe name recovered from the session identifier for elevated processes;
  window title resolved through same-exe parent processes for windowless audio
  children (browsers).

## Config files

- `config/mixers/main.json` — the live autosaved matrix (gitignored; personal).
- `config/scenes/*.json` — named level overlays (gitignored).
- `config/app-rules.json` — semi-auto assignment: `rules` (`match` → group,
  wildcards ok) and `ignore`. Tracked; seeded as a base template.
- `scripts/.routed_apps_<port>.json` — TUI sidecar tracking per-app routes (gitignored).

## Roadmap / next candidates

Phase 2 (DSP + matrix + control surfaces) is working end-to-end, including per-input
HRTF spatialization. The long-term product goal is a consumer-grade Windows 11 audio
mixer: simple enough for any gamer, streamer, or remote worker, with an optional
Advanced mode for enthusiasts who want the full routing matrix, per-app sends, DSP
chain, and API. That drives the UI (simple-by-default, Advanced toggle), installer,
background service, and auto-updater work in later phases.

Near-term open items, roughly ranked: WebSocket event stream as an alternative to
SSE; per-output HRTF virtualization / Windows Sonic replacement (needs multi-channel
output/virtual driver work); Loupedeck action artwork/icons; Electron mixer GUI
polish / packaging; driver signing + installer — the installer technology has not been chosen (NSIS,
WiX, and Inno Setup are candidates) and the driver-signing cost (EV cert + MS
attestation) is the real barrier to shipping to other users. See `docs/ROADMAP.md`.
