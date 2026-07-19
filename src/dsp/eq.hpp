#pragma once

#include <cstdint>
#include <vector>

// ── Biquad parametric EQ chain ──
//
// A minimal, real-time-safe (after prepare()) chain of biquad IIR filters.
// Coefficients are computed in double precision; per-channel state is kept so
// the chain can process interleaved float audio on the engine's audio thread.

namespace anniaudio::dsp {

enum class FilterType { Peak, LowShelf, HighShelf, LowPass, HighPass, Notch, Allpass };

struct BiquadCoeffs {
    double b0 = 0.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;
};

struct BiquadState {
    double x1 = 0.0, x2 = 0.0;
    double y1 = 0.0, y2 = 0.0;
};

class EqChain {
public:
    struct Band {
        FilterType type;
        double     freq;    // Hz
        double     gainDb;  // ignored by LPF/HPF/notch/allpass
        double     q;       // Q factor (slope S=1 for shelves)
    };

    // Add a band. Call before prepare().
    void addBand(FilterType type, double freq, double gainDb, double q);
    void clearBands();

    // Allocate per-channel state and compute coefficients.
    // Safe to call from the audio thread on first use; after that, process() is allocation-free.
    void prepare(double sampleRate, uint32_t channels);

    // Process interleaved float samples in-place.
    void process(float* interleaved, uint32_t frames, uint32_t channels) noexcept;

    size_t bandCount() const noexcept { return bands_.size(); }
    bool   prepared() const noexcept { return prepared_; }

private:
    double sampleRate_ = 0.0;
    uint32_t channels_ = 0;
    std::vector<Band> bands_;
    std::vector<BiquadCoeffs> coeffs_;
    std::vector<std::vector<BiquadState>> states_;
    bool prepared_ = false;
};

} // namespace anniaudio::dsp
