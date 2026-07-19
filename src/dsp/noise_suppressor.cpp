#include "noise_suppressor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace anniaudio::dsp {

// RNNoise is trained on 48 kHz PCM16-range float data. WASAPI shared-mode float
// streams are normalized to [-1, 1], so we scale up before inference and back down
// afterward.
static constexpr float kInt16Scale = 32768.0f;

NoiseSuppressor::~NoiseSuppressor()
{
    cleanup();
}

void NoiseSuppressor::cleanup()
{
    for (DenoiseState* st : states_) {
        if (st) rnnoise_destroy(st);
    }
    states_.clear();
    prepared_ = false;
    channels_ = 0;
    frameSize_ = 0;
}

void NoiseSuppressor::prepare(uint32_t channels)
{
    cleanup();

    if (channels == 0) return;

    frameSize_ = rnnoise_get_frame_size();
    if (frameSize_ <= 0) return;

    channels_ = channels;
    states_.resize(channels_);
    for (uint32_t c = 0; c < channels_; ++c) {
        states_[c] = rnnoise_create(nullptr);
    }

    tmpIn_.resize(frameSize_);
    tmpOut_.resize(frameSize_);

    prepared_ = true;
}

void NoiseSuppressor::process(float* interleaved, uint32_t frames, uint32_t channels)
{
    if (interleaved == nullptr || frames == 0) return;

    if (!prepared_ || channels != channels_) {
        prepare(channels);
        if (!prepared_) return;
    }

    const uint32_t blockSize = static_cast<uint32_t>(frameSize_);
    if (blockSize == 0 || frames < blockSize) return;

    const uint32_t blockCount = frames / blockSize;

    for (uint32_t b = 0; b < blockCount; ++b) {
        const uint32_t base = b * blockSize;
        for (uint32_t c = 0; c < channels; ++c) {
            DenoiseState* st = states_[c];
            if (!st) continue;

            for (uint32_t i = 0; i < blockSize; ++i) {
                tmpIn_[i] = interleaved[(base + i) * channels + c] * kInt16Scale;
            }

            rnnoise_process_frame(st, tmpOut_.data(), tmpIn_.data());

            for (uint32_t i = 0; i < blockSize; ++i) {
                float s = tmpOut_[i] / kInt16Scale;
                if (s >  1.0f) s =  1.0f;
                if (s < -1.0f) s = -1.0f;
                interleaved[(base + i) * channels + c] = s;
            }
        }
    }
}

} // namespace anniaudio::dsp
