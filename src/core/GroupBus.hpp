#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "InputProcessor.hpp"

namespace anniaudio::core {

// One group bus for a specific (group, output) pair. It reads from the 48 kHz
// stereo rings of all InputProcessors feeding the group, mixes them, applies the
// group gain/mute, and resamples to the output's sample rate and channel count.
//
// This class is not a thread; it is driven from the OutputMixer render callback.
class GroupBus {
public:
    GroupBus();
    ~GroupBus();

    void addInput(InputProcessor& input);
    void removeInput(InputProcessor& input);
    void clearInputs();

    void setGain(float gain); // linear
    void setMuted(bool muted);
    float gain() const;
    bool muted() const;

    // Peak and RMS level from the last mix() call (linear).
    float peak() const;
    float rms() const;

    // Fill `dst` (interleaved `channels` x `frames`) with the mixed group signal.
    // `rate` is the destination sample rate. The first call sizes internal scratch
    // buffers; after that it is allocation-free.
    void mix(float* dst, uint32_t frames, uint32_t rate, uint32_t channels);

    // Current inputs feeding this bus.
    std::vector<InputProcessor*> inputProcessors() const;

private:
    struct Source {
        InputProcessor* input = nullptr;
        size_t readCursor = 0;
        std::vector<float> srcBuf;
    };

    std::vector<std::shared_ptr<Source>> sources_;
    mutable std::mutex sourceMutex_;

    std::atomic<float> gain_{1.0f};
    std::atomic<bool> muted_{false};
    std::atomic<float> peak_{0.0f};
    std::atomic<float> rms_{0.0f};
};

} // namespace anniaudio::core
