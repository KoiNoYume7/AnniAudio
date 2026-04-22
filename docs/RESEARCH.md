# AnniAudio — Research Notes

Links, findings, code snippets, and open questions gathered during Phase 0. Updated as we learn things.

---

## Virtual WDM Driver

### Starting Point
Microsoft `sysvad` sample driver — a complete, working virtual audio device:
- https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad
- Implements WDM PortCls, multiple streams, render and capture
- MIT licensed
- This is what we build on. Not from scratch.

### WDK Setup
The Windows Driver Kit must match your Windows SDK version. Get the right pair from:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk

### PortCls vs AVStream
PortCls is the older framework and has far more documentation and examples. AVStream is newer but more complex. Stick with PortCls — it is what sysvad uses and it is the right choice for a virtual audio device.

### Development Signing
```powershell
# Enable test signing mode (allows self-signed drivers)
bcdedit /set testsigning on
# Reboot required after this

# Create a self-signed code signing cert
New-SelfSignedCertificate -Type CodeSigningCert -Subject "AnniAudio Dev" -CertStoreLocation Cert:\CurrentUser\My
```

### Production Signing Options
- **EV Code Signing Certificate** — required for standard kernel driver signing. Vendors: DigiCert, Sectigo, GlobalSign. Cost ~$300–500/year.
- **Microsoft Attestation Signing** — free, done through Microsoft Hardware Dev Center. Does not require WHQL certification. This is probably the right path for an open source project.
- WHQL certification is expensive and unnecessary here.

### Projects Worth Studying
- EarTrumpet — good example of Windows audio API usage in C#: https://github.com/File-New-Project/EarTrumpet
- SoundSwitch — per-app audio device switching: https://github.com/Belphemur/SoundSwitch
- OBS Studio audio pipeline — full C audio processing chain, good reference: https://github.com/obsproject/obs-studio

---

## WASAPI

### Documentation
- https://learn.microsoft.com/en-us/windows/win32/coreaudio/wasapi

### Loopback Capture
Capture whatever is playing on an output device:
```cpp
audioClient->Initialize(
    AUDCLNT_SHAREMODE_SHARED,
    AUDCLNT_STREAMFLAGS_LOOPBACK,
    hnsBufferDuration,
    0,
    pwfx,
    nullptr
);
```

### Key Interfaces
- `IAudioClient` — initialize a stream on a device
- `IAudioRenderClient` — push audio to an output device
- `IAudioCaptureClient` — pull audio from an input or loopback

### Shared vs Exclusive Mode
Shared mode is the safe default (~20–100ms latency). Exclusive mode gives ~1–10ms but locks the device — other apps cannot use it simultaneously. If another app is already in exclusive mode on a device, we cannot capture from it.

### Sample Rate Conversion
WASAPI mix format is typically 48kHz / 32-bit float. If our virtual device exposes a different format, we need a resampler. Options: Windows MFT resampler (built-in, no extra deps), or `libsamplerate` (BSD licensed): http://www.mega-nerd.com/SRC/

---

## NVIDIA RTX Effects SDK

### What It Is
The AI model behind RTX Voice and NVIDIA Broadcast, exposed as a developer SDK. Exactly what we want.

### Links
- Developer page: https://developer.nvidia.com/rtx/broadcast/audio-effects/sdk
- GitHub: https://github.com/NVIDIA/MAXINE-AR-SDK

### GPU Support
- RTX cards: native support
- GTX 10xx and newer: add this registry value to remove the GPU requirement check:
  ```
  HKLM\SYSTEM\CurrentControlSet\Services\nvlddmkm\Global\NvAFX
  DWORD MinGPUArch = 0
  ```

### Basic API Usage
```cpp
NvAFX_Handle handle;
NvAFX_CreateEffect(NVAFX_EFFECT_DENOISER, &handle);
NvAFX_SetU32(handle, NVAFX_PARAM_INPUT_SAMPLE_RATE, 48000);
NvAFX_SetU32(handle, NVAFX_PARAM_NUM_CHANNELS, 1);
NvAFX_SetString(handle, NVAFX_PARAM_MODEL_PATH, "path/to/model");
NvAFX_Load(handle);

// Per-frame processing
NvAFX_Run(handle, &inputBuffer, &outputBuffer, numSamples, numChannels);
```

### Requirements
- Input must be 48kHz, mono, float32
- May need to downsample and downmix before passing to the SDK
- Model files are ~50MB

### Model Distribution
**Resolved:** Model files are downloaded on first run, not bundled. The installer checks `%APPDATA%\AnniAudio\models\` and fetches from NVIDIA if missing. This sidesteps any EULA redistribution concern. The SDK headers and libs are linked at build time; only the runtime model files are fetched at install time.

---

## RNNoise (CPU Noise Cancellation Fallback)

### What It Is
Recurrent neural network noise suppression from Mozilla/Xiph. BSD licensed, CPU-based, no GPU required.

### Link
- https://github.com/xiph/rnnoise

### API
```c
DenoiseState *st = rnnoise_create(NULL);

// Fixed 480-sample frames (10ms at 48kHz), mono, float
float output[480];
float vad_probability = rnnoise_process_frame(st, output, input);

rnnoise_destroy(st);
```

### Notes
- Frame size is fixed: 480 samples = 10ms at 48kHz
- Mono only. Stereo input must be processed per-channel or downmixed first.
- Works well for speech. Not designed for music or non-speech audio.
- CPU usage: ~1–2% on modern hardware.
- The VAD (voice activity detection) probability returned per frame can be used to drive a UI indicator.

---

## Biquad EQ

### Reference
Audio EQ Cookbook by Robert Bristow-Johnson — the definitive reference for biquad filter design:
https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html

Read this before writing any filter code.

### Filter Types
All implemented via the same biquad structure, different coefficient formulas:
- Peaking EQ (boost/cut at a frequency)
- Low shelf
- High shelf
- Low-pass (12dB/oct)
- High-pass (12dB/oct)
- Notch
- Allpass (phase manipulation)

### Implementation Pattern
```cpp
struct BiquadCoeffs { double b0, b1, b2, a1, a2; };
struct BiquadState  { double x1, x2, y1, y2; };

double processSample(double x, const BiquadCoeffs& c, BiquadState& s) {
    double y = c.b0*x + c.b1*s.x1 + c.b2*s.x2
                      - c.a1*s.y1  - c.a2*s.y2;
    s.x2 = s.x1; s.x1 = x;
    s.y2 = s.y1; s.y1 = y;
    return y;
}
```

Always compute coefficients in `double` even if the audio buffer is `float`. Precision loss at low frequencies with `float` coefficients is audible.

---

## HRTF / Spatial Audio

### SOFA Format
SOFA (Spatially Oriented Format for Acoustics) is the standard container for HRTF impulse response datasets.
- Spec: https://www.sofaconventions.org/
- Reader: `libmysofa` — MIT licensed C library: https://github.com/hoene/libmysofa

### Free HRTF Datasets to Bundle

| Dataset | Subjects | License | Link |
|---|---|---|---|
| MIT KEMAR | 1 (dummy head) | Unrestricted | http://sound.media.mit.edu/resources/KEMAR.html |
| SADIE II | 20 subjects | Free | https://www.york.ac.uk/sadie-project/database.html |
| CIPIC | 45 subjects | Free for research | https://www.ece.ucdavis.edu/cipic/spatial-sound/hrtf-data/ |
| Listen (IRCAM) | 51 subjects | Non-commercial attribution | http://recherche.ircam.fr/equipes/salles/listen/ |
| TH Köln | 200 subjects | Free | https://www.iks.rwth-aachen.de/en/research/tools-downloads/databases/head-related-transfer-function-database/ |

Plan: bundle MIT KEMAR as default (no restrictions), bundle SADIE II as the high-quality option, support user-provided SOFA files via config.

### Convolution Implementation
FFT-based overlap-add convolution:
```
For each 512-sample frame:
  1. FFT(input)
  2. Multiply element-wise with FFT(left HRTF IR)  -> left spectrum
  3. Multiply element-wise with FFT(right HRTF IR) -> right spectrum
  4. IFFT(left spectrum) -> left output samples
  5. IFFT(right spectrum) -> right output samples
  6. Overlap-add both channels to output buffers
```

### FFT Library
Use **KissFFT** — small, BSD licensed, no dependencies, easy to integrate:
- https://github.com/mborgerding/kissfft

Do not use FFTW. It is faster but GPL licensed, which is incompatible with any future commercial path.

Alternative: **pffft** (BSD licensed, faster than KissFFT on some platforms): https://bitbucket.org/jpommier/pffft

---

## Config and JSON

### Library
nlohmann/json — header-only, MIT licensed, excellent C++ integration:
- https://github.com/nlohmann/json

Single header file. No build system integration needed. Include and use.

---

## HTTP API Server

### Library
cpp-httplib — header-only HTTP/1.1 and WebSocket server, MIT licensed:
- https://github.com/yhirose/cpp-httplib

Single header. Supports GET/POST/DELETE, WebSocket upgrade, chunked responses, and SSL if needed.

---

## Open Questions

All major pre-build decisions are now resolved. Remaining items are deferred to the phases where they become relevant.

- [x] **NVIDIA RTX Effects SDK EULA** — **Resolved:** Model files are downloaded on first run via the installer, not bundled with the release. Sidesteps redistribution entirely. The installer checks for the models, downloads them from NVIDIA if missing, and caches them in `%APPDATA%\AnniAudio\models\`. No EULA concern.
- [x] **Windows version target** — **Resolved: Win11 only.** Simplifies driver signing and testing. No Win10 support.
- [ ] **IRCAM Listen license** — Attribution required for non-commercial use. **Deferred to Phase 3** (spatial audio). Verify exact attribution text before bundling. MIT KEMAR and SADIE II are clear — start with those.
- [x] **UI stack decision** — **Resolved: Electron.** REST API backend makes this clean — Electron talks to the same API as the CLI. Phase 5.
- [x] **FFTW vs KissFFT performance delta** — **Resolved: KissFFT.** BSD licensed, sufficient performance for our frame sizes, no GPL risk. If benchmarks show a problem, pffft (also BSD) is the next option.