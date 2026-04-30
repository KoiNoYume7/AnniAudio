#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Called on the real-time audio thread per captured buffer.
// buf:      interleaved float samples — may be modified in-place (copy is made before calling)
// frames:   sample frames in this buffer (1 frame = channels samples)
// channels: channel count (matches the negotiated mix format)
using ProcessFn = std::function<void(float* buf, uint32_t frames, uint32_t channels)>;

// Routes audio from one WASAPI endpoint to another with optional DSP processing.
//
// Source (captureDeviceHint): a CAPTURE endpoint.
//   Typically the AnniAudio Cable capture endpoint — apps render to the matching
//   AnniAudio render endpoint; the kernel driver's shared cyclic buffer acts as
//   the loopback, surfacing the data on the capture side which this engine reads.
// Sink (renderDeviceHint): a RENDER endpoint, e.g. headphones or speakers.
//
// Both endpoints are opened in WASAPI shared mode.  Each device is initialised
// with its own native mix format; if they differ, linear-interpolation SRC and
// channel mapping are applied automatically.
//
// Caller is responsible for CoInitializeEx / CoUninitialize on the calling thread.
//
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // Optional DSP callback.  Defaults to passthrough if not set.
    void setProcessCallback(ProcessFn fn);

    // Start routing.  deviceHint is a case-insensitive substring of the friendly name.
    // Empty string = use the Windows default endpoint for that direction.
    bool start(const std::string& captureDeviceHint,
               const std::string& renderDeviceHint);
    void stop();

    bool     isRunning()        const;
    uint32_t sampleRate()       const;
    uint32_t channelCount()     const;
    uint64_t framesProcessed()  const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
