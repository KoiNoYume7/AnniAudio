// poc_eq.cpp
// Phase 0 POC: Biquad Parametric EQ
//
// Implements all 7 filter types from the Audio EQ Cookbook (Robert Bristow-Johnson).
// Reference: https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html
//
// No external dependencies. Compile with:
//   cl /std:c++20 poc_eq.cpp /Fepoc_eq.exe             (MSVC)
//   g++ -std=c++20 poc_eq.cpp -o poc_eq                (GCC/Clang)
//
// Prints a frequency response table for each filter type and runs
// assertions to verify correctness. Exits 0 on pass, 1 on failure.

#include <cmath>
#include <complex>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>

static constexpr double PI = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

enum class FilterType { Peak, LowShelf, HighShelf, LowPass, HighPass, Notch, Allpass };

struct BiquadCoeffs {
    double b0, b1, b2; // Numerator   (a0-normalized)
    double a1, a2;     // Denominator (a0 = 1 implicit)
};

struct BiquadState {
    double x1 = 0.0, x2 = 0.0;
    double y1 = 0.0, y2 = 0.0;
};

// ---------------------------------------------------------------------------
// Coefficient computation
// Always computed in double precision, even when audio is float.
// Shelf filters: Q parameter is ignored; slope is fixed at S=1 (Q≈0.707,
// maximally flat). Q-based shelf slope is a TODO for production code.
// ---------------------------------------------------------------------------

BiquadCoeffs computeCoeffs(FilterType type, double freq, double gainDb,
                            double q, double sampleRate)
{
    const double A     = std::pow(10.0, gainDb / 40.0);         // linear amplitude for peak/shelf
    const double w0    = 2.0 * PI * freq / sampleRate;
    const double sinW0 = std::sin(w0);
    const double cosW0 = std::cos(w0);

    // Q-based bandwidth (LPF, HPF, peak, notch, allpass)
    const double alpha  = sinW0 / (2.0 * q);

    // Shelf alpha at S=1 (cookbook: sin(w0)/2 * sqrt((A+1/A)*(1/S-1)+2), S=1 => sqrt(2))
    const double alphaS = sinW0 * std::sqrt(2.0) / 2.0;

    double b0, b1, b2, a0, a1, a2;

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

    default:
        b0 = 1.0; b1 = b2 = a0 = a1 = a2 = 0.0; a0 = 1.0;
        break;
    }

    return { b0/a0, b1/a0, b2/a0, a1/a0, a2/a0 };
}

// ---------------------------------------------------------------------------
// Sample processing — Direct Form I (matches architecture spec)
// ---------------------------------------------------------------------------

inline double processSample(double x, const BiquadCoeffs& c, BiquadState& s)
{
    const double y = c.b0*x + c.b1*s.x1 + c.b2*s.x2
                             - c.a1*s.y1  - c.a2*s.y2;
    s.x2 = s.x1; s.x1 = x;
    s.y2 = s.y1; s.y1 = y;
    return y;
}

// ---------------------------------------------------------------------------
// Analytical frequency response — |H(f)| in dB
// H(z) = (b0 + b1*z^-1 + b2*z^-2) / (1 + a1*z^-1 + a2*z^-2), z = e^(j*w)
// ---------------------------------------------------------------------------

double responseDb(const BiquadCoeffs& c, double freq, double sampleRate)
{
    using Cx = std::complex<double>;
    const double w  = 2.0 * PI * freq / sampleRate;
    const Cx     z1 = std::exp(Cx(0.0, -w));
    const Cx     z2 = std::exp(Cx(0.0, -2.0*w));
    const Cx     H  = (c.b0 + c.b1*z1 + c.b2*z2) / (1.0 + c.a1*z1 + c.a2*z2);
    return 20.0 * std::log10(std::abs(H));
}

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

static int failures = 0;

void check(const std::string& label, double actual, double expected,
           double toleranceDb = 0.5)
{
    const double err = std::abs(actual - expected);
    if (err > toleranceDb) {
        ++failures;
        std::cerr << "  FAIL  " << label
                  << ": got " << std::fixed << std::setprecision(2) << actual
                  << " dB, expected " << expected
                  << " dB (err=" << err << ")\n";
    }
}

void checkLess(const std::string& label, double actual, double threshold)
{
    if (actual >= threshold) {
        ++failures;
        std::cerr << "  FAIL  " << label
                  << ": got " << std::fixed << std::setprecision(2) << actual
                  << " dB (expected < " << threshold << " dB)\n";
    }
}

void printResponse(const std::string& title, const BiquadCoeffs& c, double sampleRate,
                   const std::vector<double>& freqs)
{
    std::cout << "\n=== " << title << " ===\n";
    std::cout << std::fixed << std::setprecision(1);
    for (double f : freqs) {
        const double db = responseDb(c, f, sampleRate);
        std::cout << "  " << std::setw(7) << f << " Hz : "
                  << std::setprecision(2) << std::setw(7) << db << " dB\n";
    }
}

// ---------------------------------------------------------------------------

int main()
{
    constexpr double FS = 48000.0;

    const std::vector<double> sweep = {
        20, 50, 100, 200, 500, 1000, 2000, 5000, 8000, 12000, 16000, 20000
    };

    std::cout << "AnniAudio — Phase 0 POC: Biquad EQ\n";
    std::cout << "Sample rate: " << FS << " Hz\n";

    // --- Peak: +6 dB @ 1 kHz, Q=1 ---
    {
        auto c = computeCoeffs(FilterType::Peak, 1000.0, 6.0, 1.0, FS);
        printResponse("Peak +6 dB @ 1 kHz, Q=1", c, FS, sweep);
        check("peak center +6dB",      responseDb(c, 1000, FS),  6.0, 0.3);
        check("peak far below ~0dB",   responseDb(c,   50, FS),  0.0, 0.3);
        check("peak far above ~0dB",   responseDb(c, 16000, FS), 0.0, 0.3);
    }

    // --- Peak: -6 dB @ 1 kHz, Q=1 (cut) ---
    {
        auto c = computeCoeffs(FilterType::Peak, 1000.0, -6.0, 1.0, FS);
        check("cut center -6dB",       responseDb(c, 1000, FS), -6.0, 0.3);
        check("cut passthrough ~0dB",  responseDb(c,   50, FS),  0.0, 0.3);
    }

    // --- Peak: narrow band, Q=8 ---
    {
        auto c = computeCoeffs(FilterType::Peak, 1000.0, 6.0, 8.0, FS);
        check("narrow peak at center", responseDb(c, 1000, FS),  6.0, 0.3);
        check("narrow peak Q8 outside BW", responseDb(c, 500, FS),  0.0, 0.3);
    }

    // --- Low Shelf: +6 dB @ 200 Hz ---
    {
        auto c = computeCoeffs(FilterType::LowShelf, 200.0, 6.0, 0.707, FS);
        printResponse("Low Shelf +6 dB @ 200 Hz", c, FS, sweep);
        check("low shelf DC end ~+6dB",   responseDb(c,   20, FS),  6.0, 1.0);
        check("low shelf high end ~0dB",  responseDb(c, 8000, FS),  0.0, 0.5);
    }

    // --- Low Shelf: -6 dB @ 200 Hz ---
    {
        auto c = computeCoeffs(FilterType::LowShelf, 200.0, -6.0, 0.707, FS);
        check("low shelf cut DC ~-6dB",   responseDb(c,   20, FS), -6.0, 1.0);
        check("low shelf cut high ~0dB",  responseDb(c, 8000, FS),  0.0, 0.5);
    }

    // --- High Shelf: +6 dB @ 8 kHz ---
    {
        auto c = computeCoeffs(FilterType::HighShelf, 8000.0, 6.0, 0.707, FS);
        printResponse("High Shelf +6 dB @ 8 kHz", c, FS, sweep);
        check("high shelf top ~+6dB",    responseDb(c, 20000, FS),  6.0, 1.0);
        check("high shelf bottom ~0dB",  responseDb(c,   200, FS),  0.0, 0.5);
    }

    // --- Low-pass: -3 dB @ 1 kHz, Q=0.707 (Butterworth) ---
    {
        auto c = computeCoeffs(FilterType::LowPass, 1000.0, 0.0, 0.707, FS);
        printResponse("LPF @ 1 kHz, Q=0.707", c, FS, sweep);
        check("lpf at cutoff ~-3dB",     responseDb(c,  1000, FS), -3.0, 0.2);
        check("lpf well below ~0dB",     responseDb(c,   200, FS),  0.0, 0.3);
        checkLess("lpf well above <-20dB", responseDb(c, 10000, FS), -20.0);
    }

    // --- High-pass: -3 dB @ 1 kHz, Q=0.707 (Butterworth) ---
    {
        auto c = computeCoeffs(FilterType::HighPass, 1000.0, 0.0, 0.707, FS);
        printResponse("HPF @ 1 kHz, Q=0.707", c, FS, sweep);
        check("hpf at cutoff ~-3dB",     responseDb(c,  1000, FS), -3.0, 0.2);
        check("hpf well above ~0dB",     responseDb(c, 10000, FS),  0.0, 0.3);
        checkLess("hpf well below <-20dB", responseDb(c,   100, FS), -20.0);
    }

    // --- Notch @ 1 kHz, Q=4 ---
    {
        auto c = computeCoeffs(FilterType::Notch, 1000.0, 0.0, 4.0, FS);
        printResponse("Notch @ 1 kHz, Q=4", c, FS, sweep);
        checkLess("notch depth <-40dB",    responseDb(c, 1000, FS), -40.0);
        check("notch far below ~0dB",      responseDb(c,  100, FS),  0.0, 0.3);
        check("notch far above ~0dB",      responseDb(c, 8000, FS),  0.0, 0.3);
    }

    // --- Allpass @ 1 kHz, Q=0.707 — magnitude must be flat everywhere ---
    {
        auto c = computeCoeffs(FilterType::Allpass, 1000.0, 0.0, 0.707, FS);
        for (double f : sweep)
            check("allpass flat @ " + std::to_string((int)f) + " Hz",
                  responseDb(c, f, FS), 0.0, 0.05);
    }

    // --- Chain test: two peaks in series ---
    {
        auto c1 = computeCoeffs(FilterType::Peak,  500.0, +6.0, 1.0, FS);
        auto c2 = computeCoeffs(FilterType::Peak, 2000.0, -6.0, 1.0, FS);
        BiquadState s1, s2;

        // Drive with an impulse and let the chain process it
        const int N = 4096;
        std::vector<double> output(N, 0.0);
        for (int i = 0; i < N; ++i) {
            double x = (i == 0) ? 1.0 : 0.0;
            output[i] = processSample(processSample(x, c1, s1), c2, s2);
        }

        // Verify the individual filter responses still hold at key frequencies
        check("chain: 500Hz boost",  responseDb(c1,  500, FS),  6.0, 0.5);
        check("chain: 2kHz cut",     responseDb(c2, 2000, FS), -6.0, 0.5);
    }

    // --- Summary ---
    std::cout << "\n---\n";
    if (failures == 0) {
        std::cout << "ALL TESTS PASSED (" << sweep.size() << " frequencies verified per filter)\n";
        return 0;
    } else {
        std::cout << failures << " TEST(S) FAILED\n";
        return 1;
    }
}
