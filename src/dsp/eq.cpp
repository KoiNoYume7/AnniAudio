#include "eq.hpp"

#include <cmath>
#include <cstring>

namespace anniaudio::dsp {

static constexpr double PI = 3.14159265358979323846;

static BiquadCoeffs computeCoeffs(FilterType type, double freq, double gainDb,
                                   double q, double sampleRate)
{
    const double A     = std::pow(10.0, gainDb / 40.0);
    const double w0    = 2.0 * PI * freq / sampleRate;
    const double sinW0 = std::sin(w0);
    const double cosW0 = std::cos(w0);
    const double alpha = sinW0 / (2.0 * q);
    const double alphaS = sinW0 * std::sqrt(2.0) / 2.0;

    double b0 = 0.0, b1 = 0.0, b2 = 0.0, a0 = 0.0, a1 = 0.0, a2 = 0.0;

    switch (type) {
    case FilterType::Peak:
        b0 =  1.0 + alpha * A;
        b1 = -2.0 * cosW0;
        b2 =  1.0 - alpha * A;
        a0 =  1.0 + alpha / A;
        a1 = -2.0 * cosW0;
        a2 =  1.0 - alpha / A;
        break;

    case FilterType::LowShelf:
        b0 =         A * ((A+1) - (A-1)*cosW0 + 2.0*std::sqrt(A)*alphaS);
        b1 =       2*A * ((A-1) - (A+1)*cosW0);
        b2 =         A * ((A+1) - (A-1)*cosW0 - 2.0*std::sqrt(A)*alphaS);
        a0 =             (A+1) + (A-1)*cosW0  + 2.0*std::sqrt(A)*alphaS;
        a1 = -2.0      * ((A-1) + (A+1)*cosW0);
        a2 =             (A+1) + (A-1)*cosW0  - 2.0*std::sqrt(A)*alphaS;
        break;

    case FilterType::HighShelf:
        b0 =         A * ((A+1) + (A-1)*cosW0 + 2.0*std::sqrt(A)*alphaS);
        b1 =      -2*A * ((A-1) + (A+1)*cosW0);
        b2 =         A * ((A+1) + (A-1)*cosW0 - 2.0*std::sqrt(A)*alphaS);
        a0 =             (A+1) - (A-1)*cosW0  + 2.0*std::sqrt(A)*alphaS;
        a1 =  2.0      * ((A-1) - (A+1)*cosW0);
        a2 =             (A+1) - (A-1)*cosW0  - 2.0*std::sqrt(A)*alphaS;
        break;

    case FilterType::LowPass:
        b0 = (1.0 - cosW0) / 2.0;
        b1 =  1.0 - cosW0;
        b2 = (1.0 - cosW0) / 2.0;
        a0 =  1.0 + alpha;
        a1 = -2.0 * cosW0;
        a2 =  1.0 - alpha;
        break;

    case FilterType::HighPass:
        b0 =  (1.0 + cosW0) / 2.0;
        b1 = -(1.0 + cosW0);
        b2 =  (1.0 + cosW0) / 2.0;
        a0 =   1.0 + alpha;
        a1 =  -2.0 * cosW0;
        a2 =   1.0 - alpha;
        break;

    case FilterType::Notch:
        b0 =  1.0;
        b1 = -2.0 * cosW0;
        b2 =  1.0;
        a0 =  1.0 + alpha;
        a1 = -2.0 * cosW0;
        a2 =  1.0 - alpha;
        break;

    case FilterType::Allpass:
        b0 =  1.0 - alpha;
        b1 = -2.0 * cosW0;
        b2 =  1.0 + alpha;
        a0 =  1.0 + alpha;
        a1 = -2.0 * cosW0;
        a2 =  1.0 - alpha;
        break;
    }

    return { b0/a0, b1/a0, b2/a0, a1/a0, a2/a0 };
}

static inline double processSample(double x, const BiquadCoeffs& c, BiquadState& s)
{
    const double y = c.b0*x + c.b1*s.x1 + c.b2*s.x2
                          - c.a1*s.y1  - c.a2*s.y2;
    s.x2 = s.x1; s.x1 = x;
    s.y2 = s.y1; s.y1 = y;
    return y;
}

void EqChain::addBand(FilterType type, double freq, double gainDb, double q)
{
    bands_.push_back({ type, freq, gainDb, q });
    prepared_ = false;
}

void EqChain::clearBands()
{
    bands_.clear();
    prepared_ = false;
}

void EqChain::prepare(double sampleRate, uint32_t channels)
{
    if (sampleRate <= 0.0 || channels == 0) return;

    sampleRate_ = sampleRate;
    channels_   = channels;
    coeffs_.clear();
    for (const auto& b : bands_) {
        coeffs_.push_back(computeCoeffs(b.type, b.freq, b.gainDb, b.q, sampleRate_));
    }

    states_.assign(channels_, std::vector<BiquadState>(coeffs_.size()));
    prepared_ = !coeffs_.empty();
}

void EqChain::process(float* interleaved, uint32_t frames, uint32_t channels) noexcept
{
    if (!prepared_ || interleaved == nullptr || frames == 0) return;
    if (channels != channels_) {
        // Re-prepare if the channel count changed mid-stream.
        prepare(sampleRate_, channels);
    }

    for (uint32_t f = 0; f < frames; ++f) {
        for (uint32_t c = 0; c < channels; ++c) {
            const size_t idx = static_cast<size_t>(f) * channels + c;
            double x = static_cast<double>(interleaved[idx]);
            auto& state = states_[c];
            for (size_t i = 0; i < coeffs_.size(); ++i) {
                x = processSample(x, coeffs_[i], state[i]);
            }
            // Clamp to float range to avoid NaN/inf blow-up.
            if (x >  1.0) x =  1.0;
            if (x < -1.0) x = -1.0;
            interleaved[idx] = static_cast<float>(x);
        }
    }
}

} // namespace anniaudio::dsp
