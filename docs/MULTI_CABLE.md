# Multi-Cable Architecture (v0.3.0)

## Overview

AnniAudio v0.3.0 introduces the ability to create **N independent virtual audio cables**, each with its own render/capture endpoint pair, isolated buffers, and independent device node in Windows.

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

The WASAPI engine (`AudioEngine`) can route audio between any pair of endpoints:

- Cable 1 Render → Cable 2 Capture (cross-cable)
- Cable 1 Render → Cable 1 Capture (loopback)
- Physical Input → Cable N Render (inject)
- Cable N Capture → Physical Output (monitor)

## Configuration

```json
{
  "cables": [
    { "id": 1, "name": "Game Audio",    "enabled": true,  "hw_id": "ROOT\\AnniAudioCable1" },
    { "id": 2, "name": "Voice Chat",    "enabled": true,  "hw_id": "ROOT\\AnniAudioCable2" },
    { "id": 3, "name": "Music Stream",  "enabled": false, "hw_id": "ROOT\\AnniAudioCable3" }
  ]
}
```

Only `enabled: true` cables are emitted into the INF and created during install.

## Limitations

- Each cable uses its own 200ms cyclic buffer. Memory usage scales linearly with N.
- The driver does not currently support dynamic add/remove at runtime (PnP surprise removal is not implemented). To change the cable count, uninstall and re-install.
