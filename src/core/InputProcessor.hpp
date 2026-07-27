#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio_utils.hpp"

namespace anniaudio::core {

// Configuration for a single InputProcessor. Mirrors the user-visible
// InputConfig in AudioMixerMatrix but lives in this header to avoid a circular
// include between InputProcessor and the matrix.
struct InputProcessorConfig {
    std::string name;
    std::string type = "device";   // "device" or "application"
    std::string source;             // endpoint name or decimal process id
    bool denoise = false;
    std::string eqPreset;
    bool spatial = false;
    float azimuth = 0.0f;
    float elevation = 0.0f;
};

// Captures one input (device or application loopback), runs the capture-side DSP
// chain (RNNoise, EQ, optional HRTF spatialization), and writes the processed
// interleaved stereo result to a ring buffer at a fixed 48 kHz processing rate.
//
// Designed for the per-input DSP refactor: one InputProcessor per input,
// consumed by one or more GroupBus instances.
class InputProcessor {
public:
    static constexpr uint32_t kProcessingRate = 48000;
    static constexpr uint32_t kProcessingChannels = 2;

    InputProcessor();
    ~InputProcessor();

    // Open the capture endpoint and prepare DSP. Must not be called while running.
    bool init(const InputProcessorConfig& cfg);

    // Start the capture thread.
    bool start();

    // Stop the capture thread and release resources.
    void stop();

    bool running() const;

    // Interleaved stereo ring at 48 kHz. Single producer (this thread); multiple
    // consumers read via readOrSilence() with their own cursor.
    MultiReaderRingBuffer& outputRing();

    // Live direction change, glitch-free. No-op if spatial is not enabled.
    void setDirection(float azimuthDeg, float elevationDeg);

    // Peak and RMS level from the last processed packet (linear, same scale as
    // the existing AudioMixer strip meters).
    float peak() const;
    float rms() const;

    // Runtime toggles and source changes are not live; they require stop/init/start.
    // The owning matrix should recreate the InputProcessor when those change.

private:
    class Impl;
    std::unique_ptr<Impl> p;
};

} // namespace anniaudio::core
