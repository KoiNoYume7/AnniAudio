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
// Source (captureDeviceHint): a CAPTURE endpoint, e.g. the output side of a VB-Cable pair
//   — apps route audio TO the matching RENDER endpoint, this engine reads it back.
// Sink   (renderDeviceHint):  a RENDER endpoint, e.g. headphones or speakers.
//
// Both endpoints are opened in WASAPI shared mode using the render device's mix format.
// If the capture device does not support that format, start() returns false with a
// helpful diagnostic printed to stderr.
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
