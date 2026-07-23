# AnniAudio Virtual WDM Driver

`driver/` contains the Windows Driver Model (WDM) / PortCls virtual audio driver
that creates `AnniAudio Cable` render/capture devices in Windows.

## What it does

- Creates one or more virtual audio cable device instances in the system.
- Each cable exposes both a **render** endpoint (apps can play into it) and a
  **capture** endpoint (AnniAudio can read from it).
- Built from Microsoft's `sysvad` sample driver (MIT license) and stripped to the
  essentials for loopback-style virtual cables.

## Files

- `AnniAudioCable.vcxproj` — Visual Studio driver project
- `AnniAudioCable.inf.template` — INF template; `scripts/generate-inf.ps1` produces
  the final `AnniAudioCable.inf` from `config/cables.json`
- `*.cpp` / `*.hpp` / `*.h` — driver source

## Build requirements

- Visual Studio 2022 with the **Windows Driver Kit (WDK)** installed
- `config/cables.json` created from `config/cables.json.example`
- A code-signing certificate for `.sys`/`.cat` (see `certs/README.md` and
  `scripts/build-driver.ps1`)

## Build

```powershell
copy config\cables.json.example config\cables.json
.\scripts\build-driver.ps1
```

If the build succeeds:

```
build\driver\release\AnniAudioCable.sys
build\driver\release\AnniAudioCable.inf
build\driver\release\AnniAudioCable.cat
```

## Development install

1. Enable test signing mode: `.\scripts\dev-mode.ps1` then reboot.
2. Import the test cert: `.\scripts\install-cert.ps1`
3. Install the driver: `.\scripts\install-driver.ps1`

## Distribution

For the driver to load on a normal Windows 11 install (Secure Boot, anti-cheat),
it must be **Microsoft attestation signed**. That requires an EV code signing
certificate and a Windows Hardware Dev Center account. See `README.md` for the
signing discussion.

## Current status

The driver builds, but is **not signed** and therefore not used by the daily
mixer path. Daily use currently relies on third-party virtual cables + WASAPI
loopback capture.
