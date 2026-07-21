#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ── HRTF binaural spatializer ──
//
// Positions a mono source in 3D around the listener by convolving it with a
// head-related impulse response (HRIR) pair loaded from a SOFA dataset
// (libmysofa) and rendered via FFT overlap-add convolution (KissFFT).
//
// Split responsibilities, mirroring EqChain / NoiseSuppressor:
//   - loadHrtf()      : one-time, NOT real-time safe (file IO, resampling, allocation).
//   - setDirection()  : cheap; a kd-tree lookup + two forward FFTs into preallocated
//                       buffers. No heap allocation, but intended to be driven from a
//                       control path, not per-sample.
//   - process()       : real-time safe, allocation-free. Mono in -> interleaved stereo.
//
// The convolution is standard overlap-add: for a block of F frames and an HRIR of
// L taps, the FFT size N covers maxBlock + L - 1, the tail of length L-1 carries
// between blocks, so any F <= maxBlock (as the mixer hands us) convolves correctly.

// Forward-declare so the header doesn't leak libmysofa/kissfft into the whole tree.
struct MYSOFA_EASY;

namespace anniaudio::dsp {

class Spatializer {
public:
    Spatializer();
    ~Spatializer();

    Spatializer(const Spatializer&) = delete;
    Spatializer& operator=(const Spatializer&) = delete;

    // Load a SOFA HRTF dataset and prepare all convolution state. libmysofa
    // resamples the dataset to sampleRate, so any source rate is supported.
    // maxBlock is the largest frame count process() will ever be handed.
    // Returns false (and stays !ready()) on any failure.
    bool loadHrtf(const std::string& sofaPath, double sampleRate, uint32_t maxBlock);

    // Point the source at (azimuth, elevation) in degrees:
    //   azimuth   0 = front, +90 = left, -90 = right, 180 = behind (counter-clockwise).
    //   elevation 0 = ear level, +90 = above, -90 = below.
    // Selects/interpolates the nearest HRIR pair and pre-transforms it.
    void setDirection(float azimuthDeg, float elevationDeg);

    // Convolve `frames` mono samples into `frames` interleaved stereo frames
    // (stereoOut must hold frames*2 floats). Real-time safe; frames must be
    // <= maxBlock passed to loadHrtf().
    void process(const float* mono, float* stereoOut, uint32_t frames) noexcept;

    bool     ready() const noexcept { return ready_; }
    uint32_t irLength() const noexcept { return irLen_; }
    uint32_t fftSize() const noexcept { return nfft_; }
    double   sampleRate() const noexcept { return sampleRate_; }
    float    azimuth() const noexcept { return azimuthDeg_; }
    float    elevation() const noexcept { return elevationDeg_; }

private:
    struct Impl;
    std::unique_ptr<Impl> p_;   // hides kiss_fftr/mysofa types from the header

    bool     ready_ = false;
    uint32_t irLen_ = 0;
    uint32_t nfft_ = 0;
    uint32_t maxBlock_ = 0;
    double   sampleRate_ = 0.0;
    float    azimuthDeg_ = 0.0f;
    float    elevationDeg_ = 0.0f;
};

} // namespace anniaudio::dsp
