# AnniAudio Loupedeck / Logi Actions Plugin

A C# plugin for the Logi Actions SDK (Loupedeck Live / Razer Stream Controller)
that controls the AnniAudio mixer entirely through its local REST + SSE control
API.

## What it does

The plugin is a pure API client — it never touches audio directly. It mirrors live
mixer state in real time and updates automatically when groups/outputs/scenes are
added, removed, or renamed.

### Actions

- **Cable Volume** dial (one per group): turn to adjust group volume, press to toggle mute.
- **Output Master** dial (one per output): turn to adjust output master volume, press to toggle mute.
- **Mute** touch button (one per group): one-tap mute toggle with live state.
- **Scene** touch button (one per saved scene): one-tap scene recall.

## Build

```powershell
dotnet build AnniAudioMixerPlugin/src/AnniAudioMixerPlugin.csproj -c Release
```

The post-build step links the output into the Logi Plugin Service and hot-reloads
it. `PluginApi.dll` must be available from `C:\Program Files\Logi\LogiPluginService\`
(net8.0 target).

## Verify load

Check `%LOCALAPPDATA%\Logi\LogiPluginService\Logs\plugin_logs\AnniAudioMixer.log`.

## Mixer must be running

The plugin talks to `http://127.0.0.1:8850`. Start the mixer first with
`.\start-mixer.bat`. If the mixer is not running, actions report an error status.
