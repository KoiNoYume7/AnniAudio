// poc_rnnoise.cpp
// Phase 0 POC: RNNoise CPU noise cancellation
//
// Verifies that:
//   1. rnnoise_create / rnnoise_process_frame / rnnoise_destroy work
//   2. White noise is measurably suppressed after processing
//   3. VAD probability stays low for pure noise (no speech)
//   4. rnnoise_get_frame_size() confirms the 480-sample contract
//
// Audio scale: RNNoise expects samples in int16 range (~[-32768, 32768]).
// No external dependencies beyond the vendored rnnoise in third_party/.
//
// Compile via CMake target: poc_rnnoise
// Exit 0 = all assertions passed, 1 = failure.

#include "rnnoise.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

static int failures = 0;

static void check(const char* label, bool cond, const char* detail = "")
{
    if (!cond) {
        ++failures;
        std::fprintf(stderr, "  FAIL  %s  %s\n", label, detail);
    }
}

static double rms(const std::vector<float>& buf)
{
    double sum = 0.0;
    for (float s : buf) sum += (double)s * s;
    return std::sqrt(sum / buf.size());
}

// ---------------------------------------------------------------------------

int main()
{
    std::printf("AnniAudio — Phase 0 POC: RNNoise\n\n");

    // --- 1. Frame size contract ---
    const int FRAME = rnnoise_get_frame_size();
    std::printf("rnnoise_get_frame_size() = %d samples\n", FRAME);
    check("frame size == 480", FRAME == 480);

    // --- 2. Create state ---
    DenoiseState* st = rnnoise_create(nullptr);
    check("rnnoise_create returned non-null", st != nullptr);
    if (!st) {
        std::printf("\nCannot continue without a valid DenoiseState.\n");
        return 1;
    }

    // --- 3. Generate 1 second of white noise in int16 range ---
    constexpr int   SAMPLE_RATE  = 48000;
    constexpr int   NUM_FRAMES   = SAMPLE_RATE / 480; // 100 frames = 1 second
    constexpr float NOISE_AMP    = 8192.0f;           // ~25% of int16 full-scale

    const int total_samples = FRAME * NUM_FRAMES;

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-NOISE_AMP, NOISE_AMP);

    std::vector<float> input(total_samples);
    for (auto& s : input) s = dist(rng);

    const double input_rms_val = rms(input);

    // --- 4. Process through RNNoise ---
    std::vector<float> output(total_samples);
    double vad_sum = 0.0;

    for (int i = 0; i < NUM_FRAMES; ++i) {
        const float* in_frame  = input.data()  + i * FRAME;
              float* out_frame = output.data() + i * FRAME;

        // rnnoise_process_frame writes result into out_frame
        float vad = rnnoise_process_frame(st, out_frame, in_frame);
        vad_sum += vad;
    }

    rnnoise_destroy(st);

    const double output_rms_val  = rms(output);
    const double avg_vad         = vad_sum / NUM_FRAMES;
    const double reduction_db    = 20.0 * std::log10(output_rms_val / input_rms_val);

    // --- 5. Print results ---
    std::printf("Input RMS :     %.1f  (%.4f normalized)\n",
                input_rms_val, input_rms_val / 32768.0);
    std::printf("Output RMS:     %.1f  (%.4f normalized)\n",
                output_rms_val, output_rms_val / 32768.0);
    std::printf("Reduction :     %.1f dB\n", reduction_db);
    std::printf("Avg VAD   :     %.3f  (0=noise, 1=speech)\n\n", avg_vad);

    // --- 6. Assertions ---
    char detail[64];

    // Noise should be reduced by at least 6 dB
    std::snprintf(detail, sizeof(detail), "(got %.1f dB, need <= -6.0 dB)", reduction_db);
    check("noise reduced >= 6 dB", reduction_db <= -6.0, detail);

    // VAD should be low for pure white noise (no speech)
    std::snprintf(detail, sizeof(detail), "(got %.3f, need < 0.20)", avg_vad);
    check("VAD low for pure noise", avg_vad < 0.20, detail);

    // Output must be finite (no NaN/inf blowup)
    bool all_finite = true;
    for (float s : output) {
        if (!std::isfinite(s)) { all_finite = false; break; }
    }
    check("output is finite (no NaN/inf)", all_finite);

    // --- 7. Summary ---
    std::printf("---\n");
    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    } else {
        std::printf("%d TEST(S) FAILED\n", failures);
        return 1;
    }
}
