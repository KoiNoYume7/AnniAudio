# certs/

This folder holds code-signing certificates for local development builds.
**Never commit .pfx or .cer files — this folder is gitignored.**

## Expected files

| File | Description |
|------|-------------|
| `AnniAudio.pfx` | PKCS#12 certificate + private key used by signtool |
| `AnniAudio.cer` | Public certificate exported from the PFX (optional, for trust store import) |

## Setup

1. Copy your `.pfx` here and rename it `AnniAudio.pfx`
2. Set the password as an environment variable (or pass to CMake):
   ```powershell
   $env:ANNI_CERT_PASSWORD = "your-password-here"
   ```
3. Run the one-time trust store import (see `scripts/install-cert.ps1`)

For CI / release builds, attestation signing via the Windows Hardware Dev Center
is used instead — no local cert needed.
