# Multi-Cable Architecture

## Overview

The AnniAudio virtual driver can create **N independent virtual audio cables**, each with its own render/capture endpoint pair, isolated buffers, and independent device node in Windows.

## Design Principles

1. **One driver, many instances** — The same `AnniAudioCable.sys` is loaded once into kernel memory. Each cable is a separate **device instance** (PnP node) created from the same INF via different hardware IDs.
2. **Per-instance isolation** — Every device instance gets its own `CMiniportWaveRT` object, which owns its own shared cyclic buffer, MDL, and position counter. Cables do not share memory.
3. **Config-driven** — The number of cables, their names, and hardware IDs are defined in `config/cables.json`. The INF is generated from a template at build time.

## How It Works

### Driver Store (One Package)

`pnputil /add-driver` stages **one** driver package (`AnniAudioCable.inf` + `AnniAudioCable.sys` + `AnniAudioCable.cat`).

### Device Instances (N Nodes)

The generated INF contains N entries in `[Standard.NTamd64]`:

```inf
[Standard.NTamd64]
%Cable1Name% = AnniAudioCable_Device, ROOT\AnniAudioCable1
%Cable2Name% = AnniAudioCable_Device, ROOT\AnniAudioCable2
```

Each entry maps a unique hardware ID to the same device installation section. `devcon install` is called N times (once per cable), creating N independent device nodes.

### Kernel-Mode Isolation

When Windows creates each device instance, it calls `AddDevice` → `StartDevice`, which creates one `CMiniportWaveRT` + `CMiniportTopology` pair per instance. The buffer members (`m_SharedBuffer`, `m_SharedMdl`, `m_BytesTransferred`) are **instance variables**, not globals.

```
Device Instance 1 (ROOT\AnniAudioCable1)
  └─ CMiniportWaveRT #1 ── m_SharedBuffer_A ── TimerDpc_A
  └─ CMiniportTopology #1

Device Instance 2 (ROOT\AnniAudioCable2)
  └─ CMiniportWaveRT #2 ── m_SharedBuffer_B ── TimerDpc_B
  └─ CMiniportTopology #2
```

### User-Mode Routing

The mixer (`AudioMixerMatrix`) can route any capture endpoint to any render endpoint, which includes the virtual cables created by this driver:

- Cable 1 Render → Cable 2 Capture (cross-cable): an app plays into Cable 1; the mixer captures Cable 1 and sends it to Cable 2's render endpoint.
- Physical Input → Cable N Render (inject): a microphone or line-in is routed into a virtual cable so other apps can capture it.
- Cable N Capture → Physical Output (monitor): a virtual cable's capture endpoint is mixed to speakers/headphones.
- Cable N Render → Cable N Capture (loopback) is also technically reachable, but it creates a feedback loop and is not a normal use case.

The legacy `AudioEngine` single source→output engine and the `route_cli process` command were removed; the live mixer uses `AudioMixerMatrix`.

## Configuration

```json
{
  "cables": [
    { "id": 1, "name": "Game Audio",    "enabled": true,  "hw_id": "ROOT\\AnniAudioCable1", "endpoint_name": "Game Audio" },
    { "id": 2, "name": "Voice Chat",    "enabled": true,  "hw_id": "ROOT\\AnniAudioCable2", "endpoint_name": "Voice Chat" },
    { "id": 3, "name": "Music Stream",  "enabled": false, "hw_id": "ROOT\\AnniAudioCable3", "endpoint_name": "Music Stream" }
  ]
}
```

- `id` — cable number, used to generate INF variable names.
- `name` — display name and default `endpoint_name` if `endpoint_name` is omitted.
- `enabled` — only `true` cables are emitted into the INF and installed.
- `hw_id` — unique PnP hardware ID for this device instance.
- `endpoint_name` — friendly name shown for the endpoint; defaults to `name`.

`config/cables.json.example` also contains a `settings` object with sample rate,
bit depth, channels, and `allow_96k`. These fields are reserved but not currently
parsed by `scripts/generate-inf.ps1`.

## Limitations

- Each cable uses its own 200ms cyclic buffer. Memory usage scales linearly with N.
- The driver does not currently support dynamic add/remove at runtime (PnP surprise removal is not implemented). To change the cable count, uninstall and re-install.
