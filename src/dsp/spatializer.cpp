#include "spatializer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "kiss_fftr.h"
#include "mysofa.h"

namespace anniaudio::dsp {

namespace {
// next power of two >= n (n >= 1)
uint32_t nextPow2(uint32_t n) {
    uint32_t p = 1;
    while (p < n) p <<= 1;
    return p;
}
} // namespace

struct Spatializer::Impl {
    MYSOFA_EASY* easy = nullptr;

    kiss_fftr_cfg fwd = nullptr;   // real -> complex, size nfft
    kiss_fftr_cfg inv = nullptr;   // complex -> real, size nfft

    uint32_t nfft = 0;
    uint32_t bins = 0;             // nfft/2 + 1
    uint32_t tail = 0;             // effective IR length - 1 (overlap carried between blocks)
    uint32_t irLen = 0;            // raw HRIR taps from the dataset
    uint32_t delayMax = 0;         // headroom (samples) for per-ear onset delay

    // Frequency-domain HRIRs for the current direction.
    std::vector<kiss_fft_cpx> Hl, Hr;

    // Preallocated scratch (process() touches only these — no heap on the audio thread).
    std::vector<float>        inBuf;      // nfft, zero-padded input block
    std::vector<kiss_fft_cpx> X;          // bins, input spectrum
    std::vector<kiss_fft_cpx> Yl, Yr;     // bins, per-ear product
    std::vector<float>        yl, yr;     // nfft, per-ear time output
    std::vector<float>        overlapL, overlapR;  // tail, carried between blocks

    // setDirection() scratch (not on the audio thread).
    std::vector<float>        irL, irR;   // irLen raw taps
    std::vector<float>        padL, padR; // nfft, delay-placed + zero-padded

    ~Impl() {
        if (fwd) kiss_fftr_free(fwd);
        if (inv) kiss_fftr_free(inv);
        if (easy) mysofa_close(easy);
    }
};

Spatializer::Spatializer() : p_(std::make_unique<Impl>()) {}
Spatializer::~Spatializer() = default;

bool Spatializer::loadHrtf(const std::string& sofaPath, double sampleRate, uint32_t maxBlock) {
    ready_ = false;
    if (sampleRate <= 0.0 || maxBlock == 0) return false;

    int filterLen = 0, err = 0;
    MYSOFA_EASY* easy =
        mysofa_open(sofaPath.c_str(), static_cast<float>(sampleRate), &filterLen, &err);
    if (!easy || err != MYSOFA_OK || filterLen <= 0) {
        std::fprintf(stderr, "[spatializer] mysofa_open('%s') failed: err=%d\n",
                     sofaPath.c_str(), err);
        if (easy) mysofa_close(easy);
        return false;
    }

    auto& I = *p_;
    if (I.easy) mysofa_close(I.easy);
    I.easy = easy;
    I.irLen = static_cast<uint32_t>(filterLen);
    // Onset-delay headroom: libmysofa may return a per-ear delay separate from the
    // IR taps (this is the broadband ITD). Reserve ~2 ms so we can place the IR at
    // that offset and reproduce the interaural time difference.
    I.delayMax = static_cast<uint32_t>(std::ceil(sampleRate * 0.002));
    const uint32_t effLen = I.irLen + I.delayMax;   // longest realizable impulse response
    I.tail = effLen - 1;

    I.nfft = nextPow2(maxBlock + effLen - 1);
    I.bins = I.nfft / 2 + 1;

    if (I.fwd) { kiss_fftr_free(I.fwd); I.fwd = nullptr; }
    if (I.inv) { kiss_fftr_free(I.inv); I.inv = nullptr; }
    I.fwd = kiss_fftr_alloc(static_cast<int>(I.nfft), 0, nullptr, nullptr);
    I.inv = kiss_fftr_alloc(static_cast<int>(I.nfft), 1, nullptr, nullptr);
    if (!I.fwd || !I.inv) {
        std::fprintf(stderr, "[spatializer] kiss_fftr_alloc(%u) failed\n", I.nfft);
        return false;
    }

    I.Hl.assign(I.bins, {0, 0});
    I.Hr.assign(I.bins, {0, 0});
    I.inBuf.assign(I.nfft, 0.0f);
    I.X.assign(I.bins, {0, 0});
    I.Yl.assign(I.bins, {0, 0});
    I.Yr.assign(I.bins, {0, 0});
    I.yl.assign(I.nfft, 0.0f);
    I.yr.assign(I.nfft, 0.0f);
    I.overlapL.assign(I.tail, 0.0f);
    I.overlapR.assign(I.tail, 0.0f);
    I.irL.assign(I.irLen, 0.0f);
    I.irR.assign(I.irLen, 0.0f);
    I.padL.assign(I.nfft, 0.0f);
    I.padR.assign(I.nfft, 0.0f);

    nfft_ = I.nfft;
    irLen_ = I.irLen;
    maxBlock_ = maxBlock;
    sampleRate_ = sampleRate;
    ready_ = true;

    setDirection(0.0f, 0.0f);   // default: straight ahead
    return true;
}

void Spatializer::setDirection(float azimuthDeg, float elevationDeg) {
    if (!ready_) return;
    auto& I = *p_;

    // Spherical (az, el, radius) -> cartesian in libmysofa's convention.
    // Unit radius is fine: KEMAR is a single-radius set, so the kd-tree picks the
    // nearest measurement by angle.
    float coord[3] = {azimuthDeg, elevationDeg, 1.0f};
    mysofa_s2c(coord);

    float delayL = 0.0f, delayR = 0.0f;
    mysofa_getfilter_float(I.easy, coord[0], coord[1], coord[2],
                           I.irL.data(), I.irR.data(), &delayL, &delayR);

    // Place each ear's IR at its (clamped, integer) onset delay so the interaural
    // time difference survives. Sub-sample delay is dropped for now.
    auto placeAndTransform = [&](const std::vector<float>& ir, float delay,
                                 std::vector<float>& pad, std::vector<kiss_fft_cpx>& H) {
        std::fill(pad.begin(), pad.end(), 0.0f);
        int off = static_cast<int>(std::lround(delay));
        if (off < 0) off = 0;
        if (off > static_cast<int>(I.delayMax)) off = static_cast<int>(I.delayMax);
        for (uint32_t i = 0; i < I.irLen; ++i)
            pad[off + i] = ir[i];
        kiss_fftr(I.fwd, pad.data(), H.data());
    };
    placeAndTransform(I.irL, delayL, I.padL, I.Hl);
    placeAndTransform(I.irR, delayR, I.padR, I.Hr);

    azimuthDeg_ = azimuthDeg;
    elevationDeg_ = elevationDeg;

    // A direction change discards continuity; clear the tail to avoid a click.
    std::fill(I.overlapL.begin(), I.overlapL.end(), 0.0f);
    std::fill(I.overlapR.begin(), I.overlapR.end(), 0.0f);
}

void Spatializer::process(const float* mono, float* stereoOut, uint32_t frames) noexcept {
    if (!ready_ || frames == 0) return;
    auto& I = *p_;
    if (frames > maxBlock_) frames = maxBlock_;   // contract violation guard

    // Zero-padded input block -> spectrum.
    std::memset(I.inBuf.data(), 0, I.nfft * sizeof(float));
    std::memcpy(I.inBuf.data(), mono, frames * sizeof(float));
    kiss_fftr(I.fwd, I.inBuf.data(), I.X.data());

    // Per-ear complex multiply in the frequency domain.
    for (uint32_t k = 0; k < I.bins; ++k) {
        const kiss_fft_cpx x = I.X[k];
        const kiss_fft_cpx hl = I.Hl[k];
        const kiss_fft_cpx hr = I.Hr[k];
        I.Yl[k].r = x.r * hl.r - x.i * hl.i;
        I.Yl[k].i = x.r * hl.i + x.i * hl.r;
        I.Yr[k].r = x.r * hr.r - x.i * hr.i;
        I.Yr[k].i = x.r * hr.i + x.i * hr.r;
    }

    kiss_fftri(I.inv, I.Yl.data(), I.yl.data());
    kiss_fftri(I.inv, I.Yr.data(), I.yr.data());

    // kiss inverse FFT is unnormalized.
    const float norm = 1.0f / static_cast<float>(I.nfft);

    // Overlap-add: fold the carried tail into the block head, emit `frames`, then
    // save the new tail. Works for any frames >= 1 (including frames < tail).
    const uint32_t tail = I.tail;
    for (uint32_t i = 0; i < tail; ++i) {
        I.yl[i] = I.yl[i] * norm + I.overlapL[i];
        I.yr[i] = I.yr[i] * norm + I.overlapR[i];
    }
    for (uint32_t i = tail; i < frames + tail; ++i) {
        I.yl[i] *= norm;
        I.yr[i] *= norm;
    }
    for (uint32_t n = 0; n < frames; ++n) {
        stereoOut[2 * n]     = I.yl[n];
        stereoOut[2 * n + 1] = I.yr[n];
    }
    for (uint32_t i = 0; i < tail; ++i) {
        I.overlapL[i] = I.yl[frames + i];
        I.overlapR[i] = I.yr[frames + i];
    }
}

} // namespace anniaudio::dsp
