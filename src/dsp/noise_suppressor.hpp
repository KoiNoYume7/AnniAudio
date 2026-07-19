#pragma once

#include <cstdint>
#include <vector>

#include "rnnoise.h"

// ── Real-time RNNoise wrapper ──
//
// Processes interleaved float audio one RNNoise frame (480 samples, 10 ms @ 48 kHz)
// at a time, per channel. Designed to run on the engine's audio thread after
// prepare(). If the incoming buffer size is not a multiple of the frame size, only
// complete frames are processed and the tail is left unprocessed.

namespace anniaudio::dsp {

class NoiseSuppressor {
public:
    NoiseSuppressor() = default;
    ~NoiseSuppressor();

    NoiseSuppressor(const NoiseSuppressor&) = delete;
    NoiseSuppressor& operator=(const NoiseSuppressor&) = delete;

    // Allocate a DenoiseState per channel. Real-time safe after this returns.
    void prepare(uint32_t channels);

    // Process interleaved float samples in-place.
    void process(float* interleaved, uint32_t frames, uint32_t channels);

    bool prepared() const noexcept { return prepared_; }
    int  frameSize() const noexcept { return frameSize_; }

private:
    void cleanup();

    int frameSize_ = 0;
    uint32_t channels_ = 0;
    bool prepared_ = false;
    std::vector<DenoiseState*> states_;
    std::vector<float> tmpIn_;
    std::vector<float> tmpOut_;
};

} // namespace anniaudio::dsp
