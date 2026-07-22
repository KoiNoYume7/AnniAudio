// poc_hrtf — Phase 3 Stage A proof of concept and verification harness.
//
// Proves the HRTF path in isolation before it touches the real-time mixer:
//   1. Loads the MIT KEMAR SOFA dataset via libmysofa (resampled to 48 kHz).
//   2. Verifies the FFT overlap-add convolution engine is block-size invariant
//      (same audio out regardless of how the stream is chunked).
//   3. Verifies the engine reproduces the intended HRIR (impulse response probe
//      vs the raw filter libmysofa hands back).
//   4. Verifies the result is physically spatial: interaural level and time
//      differences move the right way as the source circles the head.
//   5. Writes a binaural WAV of a source orbiting the listener, so a human can
//      confirm the last, un-automatable part of the gate: does it sound 3D?
//
// Exit code 0 = all automated checks passed.

#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "spatializer.hpp"
#include "mysofa.h"   // transitively available via audio_dsp -> mysofa

using anniaudio::dsp::Spatializer;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kMaxBlock = 512;

int g_failures = 0;
void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_failures;
}

// Deterministic pseudo-random mono noise in [-1, 1] (no <random> — keep it portable).
std::vector<float> makeNoise(size_t n, uint32_t seed = 12345) {
    std::vector<float> v(n);
    uint32_t s = seed;
    for (size_t i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        v[i] = (static_cast<float>(s >> 8) / 8388608.0f) - 1.0f;  // ~[-1,1]
    }
    return v;
}

// Run a whole mono signal through the spatializer in fixed-size chunks.
std::vector<float> runChunked(Spatializer& sp, const std::vector<float>& mono, uint32_t block) {
    std::vector<float> out(mono.size() * 2, 0.0f);
    std::vector<float> stereo(block * 2);
    for (size_t i = 0; i < mono.size(); i += block) {
        uint32_t f = static_cast<uint32_t>(std::min<size_t>(block, mono.size() - i));
        sp.process(mono.data() + i, stereo.data(), f);
        std::memcpy(out.data() + i * 2, stereo.data(), f * 2 * sizeof(float));
    }
    return out;
}

// Capture the per-ear impulse response the engine actually produces at the
// current direction (feed a unit impulse, then flush zeros through the tail).
void captureIR(Spatializer& sp, uint32_t len, std::vector<float>& left, std::vector<float>& right) {
    left.assign(len, 0.0f);
    right.assign(len, 0.0f);
    sp.reset();   // start from a clean tail so the captured IR isn't contaminated
    const uint32_t block = 256;
    std::vector<float> in(block, 0.0f), out(block * 2, 0.0f);
    uint32_t produced = 0;
    bool first = true;
    while (produced < len) {
        std::fill(in.begin(), in.end(), 0.0f);
        if (first) { in[0] = 1.0f; first = false; }
        sp.process(in.data(), out.data(), block);
        for (uint32_t n = 0; n < block && produced + n < len; ++n) {
            left[produced + n]  = out[2 * n];
            right[produced + n] = out[2 * n + 1];
        }
        produced += block;
    }
}

double rms(const std::vector<float>& v) {
    double s = 0.0;
    for (float x : v) s += static_cast<double>(x) * x;
    return std::sqrt(s / (v.empty() ? 1 : v.size()));
}

// First sample index whose magnitude exceeds `frac` of the signal peak — an
// onset estimate, used as an interaural time-difference proxy.
int onsetIndex(const std::vector<float>& v, double frac = 0.15) {
    double peak = 0.0;
    for (float x : v) peak = std::max(peak, std::fabs((double)x));
    double thr = peak * frac;
    for (size_t i = 0; i < v.size(); ++i)
        if (std::fabs((double)v[i]) >= thr) return static_cast<int>(i);
    return -1;
}

void writeWavStereo16(const std::string& path, const std::vector<float>& interleaved, uint32_t sr) {
    const uint32_t frames = static_cast<uint32_t>(interleaved.size() / 2);
    const uint16_t ch = 2, bits = 16;
    const uint32_t byteRate = sr * ch * bits / 8;
    const uint16_t blockAlign = ch * bits / 8;
    const uint32_t dataBytes = frames * blockAlign;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::printf("  (could not open %s for writing)\n", path.c_str()); return; }
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(ch); w32(sr); w32(byteRate); w16(blockAlign); w16(bits);
    std::fwrite("data", 1, 4, f); w32(dataBytes);
    for (float s : interleaved) {
        float c = std::max(-1.0f, std::min(1.0f, s));
        int16_t v = static_cast<int16_t>(std::lround(c * 32767.0f));
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
    std::printf("  wrote %s (%u frames, %.2fs)\n", path.c_str(), frames, frames / (double)sr);
}

} // namespace

int main(int argc, char** argv) {
    std::string sofa = (argc > 1) ? argv[1] : "assets/hrtf/mit_kemar.sofa";
    std::string outWav = (argc > 2) ? argv[2] : "hrtf_orbit_48k.wav";

    std::printf("poc_hrtf — HRTF spatializer verification\n");
    std::printf("  SOFA: %s\n", sofa.c_str());

    Spatializer sp;
    if (!sp.loadHrtf(sofa, kSampleRate, kMaxBlock)) {
        std::printf("FAIL: could not load HRTF dataset.\n");
        std::printf("  Pass the SOFA path as argv[1], or run from the repo root so\n"
                    "  'assets/hrtf/mit_kemar.sofa' resolves.\n");
        return 2;
    }
    std::printf("  loaded: irLen=%u taps, fftSize=%u, rate=%.0f Hz\n\n",
                sp.irLength(), sp.fftSize(), sp.sampleRate());

    // ── Test 1: block-size invariance of the overlap-add engine ──
    std::printf("Test 1 — overlap-add is block-size invariant\n");
    {
        auto noise = makeNoise(4000);
        sp.setDirection(30.0f, 0.0f);
        sp.reset(); auto a = runChunked(sp, noise, 512);
        sp.reset(); auto b = runChunked(sp, noise, 100);
        sp.reset(); auto c = runChunked(sp, noise, 337);   // deliberately not a divisor
        double maxd = 0.0;
        for (size_t i = 0; i < a.size(); ++i) {
            maxd = std::max(maxd, (double)std::fabs(a[i] - b[i]));
            maxd = std::max(maxd, (double)std::fabs(a[i] - c[i]));
        }
        std::printf("  max sample delta across block sizes 512/100/337: %.3e\n", maxd);
        check(maxd < 1e-4, "chunking does not change the output");
    }

    // ── Test 2: engine reproduces the intended HRIR ──
    std::printf("Test 2 — impulse response matches the dataset filter\n");
    {
        const float az = 40.0f, el = 0.0f;
        sp.setDirection(az, el);
        std::vector<float> capL, capR;
        captureIR(sp, sp.irLength() + 128, capL, capR);

        // Independently ask libmysofa for the same filter and place it as the
        // engine does (integer onset delay), then compare.
        int flen = 0, err = 0;
        MYSOFA_EASY* easy = mysofa_open(sofa.c_str(), (float)kSampleRate, &flen, &err);
        bool ok = (easy && err == MYSOFA_OK && flen == (int)sp.irLength());
        if (ok) {
            std::vector<float> irL(flen), irR(flen);
            float dL = 0, dR = 0;
            float coord[3] = {az, el, 1.0f};
            mysofa_s2c(coord);
            mysofa_getfilter_float(easy, coord[0], coord[1], coord[2],
                                   irL.data(), irR.data(), &dL, &dR);
            int offL = std::max(0, (int)std::lround(dL));
            int offR = std::max(0, (int)std::lround(dR));
            double maxdL = 0.0, maxdR = 0.0;
            for (int i = 0; i < flen; ++i) {
                if (offL + i < (int)capL.size()) maxdL = std::max(maxdL, (double)std::fabs(capL[offL + i] - irL[i]));
                if (offR + i < (int)capR.size()) maxdR = std::max(maxdR, (double)std::fabs(capR[offR + i] - irR[i]));
            }
            std::printf("  max IR delta L=%.3e R=%.3e (delays L=%.2f R=%.2f)\n", maxdL, maxdR, dL, dR);
            check(maxdL < 1e-3 && maxdR < 1e-3, "convolved impulse == dataset HRIR");
        } else {
            check(false, "second mysofa_open for cross-check");
        }
        if (easy) mysofa_close(easy);
    }

    // ── Test 3: interaural level and time differences are physically correct ──
    std::printf("Test 3 — spatial cues move with azimuth\n");
    {
        struct Probe { float az; double ildDb; int itd; };
        std::vector<Probe> probes;
        for (float az : {-90.f, -30.f, 0.f, 30.f, 90.f}) {
            sp.setDirection(az, 0.0f);
            std::vector<float> l, r;
            captureIR(sp, sp.irLength() + 128, l, r);
            double ild = 20.0 * std::log10((rms(l) + 1e-12) / (rms(r) + 1e-12));
            int itd = onsetIndex(r) - onsetIndex(l);   // >0 => left ear leads
            probes.push_back({az, ild, itd});
            std::printf("  az=%+5.0f  ILD=%+6.2f dB  onset(R-L)=%+d samp\n", az, ild, itd);
        }
        // Left source (+90): left ear louder and earlier.
        const Probe& left = probes.front();   // -90
        const Probe& right = probes.back();    // +90
        const Probe& front = probes[2];        // 0
        check(right.ildDb > 3.0, "source on the left is louder in the left ear");
        check(left.ildDb  < -3.0, "source on the right is louder in the right ear");
        check(std::fabs(front.ildDb) < 3.0, "front source is near level-balanced");
        check(right.itd > 0, "source on the left reaches the left ear first");
        check(left.itd  < 0, "source on the right reaches the right ear first");
    }

    // ── Diagnostic: how strong is each spatial cue in THIS dataset? ──
    // Front/back and up/down have no ILD/ITD — they live entirely in the spectral
    // difference between HRIRs. This quantifies how much signal is actually there,
    // which is exactly what governs whether a listener can hear those directions.
    std::printf("Diagnostic — relative HRIR difference between directions\n");
    {
        auto irDiff = [&](float az1, float el1, float az2, float el2) {
            std::vector<float> l1, r1, l2, r2;
            sp.setDirection(az1, el1); captureIR(sp, sp.irLength() + 128, l1, r1);
            sp.setDirection(az2, el2); captureIR(sp, sp.irLength() + 128, l2, r2);
            double dn = 0, en = 0;
            for (size_t i = 0; i < l1.size(); ++i) {
                dn += (l1[i]-l2[i])*(l1[i]-l2[i]) + (r1[i]-r2[i])*(r1[i]-r2[i]);
                en += l1[i]*l1[i] + r1[i]*r1[i] + l2[i]*l2[i] + r2[i]*r2[i];
            }
            return 100.0 * std::sqrt(dn / (en + 1e-12));
        };
        std::printf("  left  vs right  (az +90 vs -90): %6.1f%%   <- the strong cue\n", irDiff(90,0,-90,0));
        std::printf("  front vs back   (az   0 vs 180): %6.1f%%   <- the weak one you noticed\n", irDiff(0,0,180,0));
        std::printf("  ear   vs above  (el   0 vs +60): %6.1f%%   <- elevation cue\n", irDiff(0,0,0,60));
    }

    // ── Human gate 2: discrete anchored positions (incl. elevation) ──
    // A held position gives the ear far longer to judge front/back and height than
    // a fast sweep does. Listen for whether Front and Behind sound different at all.
    std::printf("Test 5 — render discrete anchored positions\n");
    {
        struct Anchor { const char* name; float az, el; };
        const Anchor anchors[] = {
            {"FRONT",   0.f,   0.f}, {"RIGHT",  -90.f,  0.f},
            {"BEHIND",180.f,   0.f}, {"LEFT",    90.f,  0.f},
            {"ABOVE",   0.f,  60.f}, {"BELOW",   0.f, -30.f},
            {"FRONT",   0.f,   0.f},
        };
        const double hold = 1.3, gap = 0.35;
        const uint32_t block = 256;
        std::vector<float> out;
        for (const Anchor& a : anchors) {
            std::printf("    %5.1fs  %s\n", out.size() / (2.0 * kSampleRate), a.name);
            sp.setDirection(a.az, a.el);
            sp.reset();   // clean start per discrete anchor
            uint32_t hn = (uint32_t)(hold * kSampleRate);
            auto burst = makeNoise(hn, 4242);
            for (uint32_t i = 0; i < hn; ++i) {
                double t = i / kSampleRate;
                double env = 0.5 * (1.0 - std::cos(2.0 * M_PI * std::fmod(t, 0.325) / 0.325));
                burst[i] *= (float)(0.3 * env);
            }
            std::vector<float> stereo(block * 2);
            for (uint32_t i = 0; i < hn; i += block) {
                uint32_t f = std::min(block, hn - i);
                sp.process(burst.data() + i, stereo.data(), f);
                out.insert(out.end(), stereo.begin(), stereo.begin() + f * 2);
            }
            out.insert(out.end(), (size_t)(gap * kSampleRate) * 2, 0.0f);   // silence gap
        }
        float pk = 1e-9f;
        for (float s : out) pk = std::max(pk, std::fabs(s));
        for (float& s : out) s *= 0.708f / pk;
        writeWavStereo16("hrtf_anchors_48k.wav", out, (uint32_t)kSampleRate);
        std::printf("  ^ each position is held ~1.3s. Can you tell FRONT from BEHIND?\n");
    }

    // ── Human gate: binaural orbit for listening ──
    std::printf("Test 4 — render a binaural orbit for listening\n");
    {
        const double dur = 6.0;
        const uint32_t total = static_cast<uint32_t>(dur * kSampleRate);
        const uint32_t block = 256;
        // Source: repeating filtered-noise bursts (broadband => strong HRTF cues).
        auto src = makeNoise(total, 777);
        for (uint32_t i = 0; i < total; ++i) {
            double t = i / kSampleRate;
            double env = 0.5 * (1.0 - std::cos(2.0 * M_PI * std::fmod(t, 0.5) / 0.5)); // 2 Hz pulses
            src[i] *= (float)(0.3 * env);   // headroom: HRTF gain peaks must not clip
        }
        std::vector<float> out(total * 2, 0.0f);
        std::vector<float> stereo(block * 2);
        for (uint32_t i = 0; i < total; i += block) {
            double t = i / kSampleRate;
            float az = (float)std::fmod(360.0 * (t / dur) * 2.0, 360.0); // two full laps
            sp.setDirection(az, 0.0f);
            uint32_t f = std::min(block, total - i);
            sp.process(src.data() + i, stereo.data(), f);
            std::memcpy(out.data() + i * 2, stereo.data(), f * 2 * sizeof(float));
        }
        // Peak-normalize to -3 dBFS so the listening aid never hard-clips
        // (broadband noise has a high crest factor).
        float pk = 1e-9f;
        for (float s : out) pk = std::max(pk, std::fabs(s));
        float g = 0.708f / pk;   // -3 dBFS
        for (float& s : out) s *= g;
        writeWavStereo16(outWav, out, (uint32_t)kSampleRate);
        std::printf("  ^ play this on headphones: the pulses should circle your head.\n");
    }

    std::printf("\n%s (%d automated check%s failed)\n",
                g_failures == 0 ? "ALL AUTOMATED CHECKS PASSED" : "SOME CHECKS FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
