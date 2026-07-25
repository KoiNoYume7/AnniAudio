# AnniAudio — Scripts and Batch Files

Most day-to-day operations are wrapped in small batch files at the repo root or
PowerShell scripts under `scripts/`.

## Day-to-day (root `.bat` files)

| File | What it does |
|---|---|
| `start-mixer.bat` | Launch `route_cli mixer config/mixers/main.json` on the default control API port (`8850`) |
| `stop-mixer.bat` | Force-stop any running `route_cli.exe` (autosave means state is preserved) |
| `mixer-tui.bat` | Start the curses TUI, installing `windows-curses` / `winappaudiorouter` if missing |
| `install-autostart.bat` | Add the mixer to the current user's `HKCU\...\Run` registry key |
| `uninstall-autostart.bat` | Remove the autostart registry entry |

## Driver build / signing / install (`scripts/*.ps1`)

| File | What it does |
|---|---|
| `scripts/build-driver.ps1` | Generate INF, build the driver project, sign `.sys`/`.cat` with the configured certificate |
| `scripts/install-cert.ps1` | Import the self-signed test certificate into the trust stores |
| `scripts/install-driver.ps1` | Use `devcon` to install each configured cable device |
| `scripts/uninstall-driver.ps1` | Disable/remove installed driver devices |
| `scripts/generate-inf.ps1` | Generate `AnniAudioCable.inf` from `config/cables.json` and the template |

## Windows mode toggles

| File | What it does |
|---|---|
| `scripts/dev-mode.ps1` | Enable Windows test signing mode (reboot required) |
| `scripts/gaming-mode.ps1` | Disable test signing mode (reboot required) |
| `scripts/restore-default.ps1` | Set a chosen render device as the default endpoint |
| `scripts/toggle-device.ps1` | Enable or disable an audio endpoint by name |

## Diagnostics

| File | What it does |
|---|---|
| `scripts/diagnose-audio.ps1` | Print endpoint state, default device, and cable assignments for troubleshooting |

## Shared library

| File | What it does |
|---|---|
| `scripts/lib/config.ps1` | Common path helpers used by the build/driver scripts (see `Get-AnniConfigPath`, `Get-AnniBuildDir`, `Get-DevConPath`, `Get-CertificateThumbprint`) |
| `scripts/lib/AnniLog.psd1` / `scripts/lib/AnniLog.psm1` | Logging module imported by `build-driver.ps1` for structured build output |

## Certificates

Code signing uses `certs/AnniAudio.pfx` for the driver and `certs/AnniAudio.cer` for the
thumbprint lookup. See `certs/README.md`.
