// poc_wasapi.cpp
// Phase 0 POC: WASAPI loopback capture + render
//
// Self-contained test — no virtual driver needed:
//   1. Enumerates and prints active render devices
//   2. Renders a 1 kHz sine wave to the default output device
//   3. Simultaneously captures via WASAPI loopback on the same device
//   4. Verifies captured RMS is non-trivial (audio actually flowed through engine)
//
// Compile via CMake target: poc_wasapi
// Links: ole32.lib avrt.lib (via #pragma comment)
// Exit 0 = all assertions passed, 1 = failure

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmreg.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

using Microsoft::WRL::ComPtr;

static constexpr double PI = 3.14159265358979323846;

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT without pulling in ks.h
static const GUID SUBFMT_IEEE_FLOAT =
    {0x00000003, 0x0000, 0x0010, {0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string wideToUtf8(const wchar_t* w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::string deviceName(IMMDevice* dev)
{
    ComPtr<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return "<unknown>";
    PROPVARIANT pv; PropVariantInit(&pv);
    props->GetValue(PKEY_Device_FriendlyName, &pv);
    std::string name = (pv.vt == VT_LPWSTR) ? wideToUtf8(pv.pwszVal) : "<unknown>";
    PropVariantClear(&pv);
    return name;
}

static bool isMixFormatFloat(const WAVEFORMATEX* fmt)
{
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        return IsEqualGUID(
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat,
            SUBFMT_IEEE_FLOAT);
    }
    return false;
}

static double computeRMS(const std::vector<float>& buf)
{
    double sum = 0.0;
    for (float s : buf) sum += (double)s * s;
    return buf.empty() ? 0.0 : std::sqrt(sum / buf.size());
}

// ---------------------------------------------------------------------------
// RAII wrapper for CoTaskMemFree'd WAVEFORMATEX*
// ---------------------------------------------------------------------------
struct WaveFormatDeleter { void operator()(WAVEFORMATEX* p){ CoTaskMemFree(p); } };
using WaveFormatPtr = std::unique_ptr<WAVEFORMATEX, WaveFormatDeleter>;

// ---------------------------------------------------------------------------
// Main logic (called from main after CoInitializeEx)
// ---------------------------------------------------------------------------
static int run()
{
    int failures = 0;
    HRESULT hr = S_OK;

    auto FAIL_IF = [&](bool cond, const char* msg, HRESULT res = S_OK) -> bool {
        if (cond) {
            ++failures;
            if (FAILED(res)) std::fprintf(stderr, "  FAIL  %s  hr=0x%08X\n", msg, (unsigned)res);
            else             std::fprintf(stderr, "  FAIL  %s\n", msg);
        }
        return cond;
    };

    // --- Device enumerator ---
    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAIL_IF(FAILED(hr), "CoCreateInstance(MMDeviceEnumerator)", hr)) return failures;

    // --- 1. List active render endpoints ---
    std::printf("=== Active render endpoints ===\n");
    {
        ComPtr<IMMDeviceCollection> col;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
            UINT count = 0; col->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                ComPtr<IMMDevice> dev; col->Item(i, &dev);
                std::printf("  [%u] %s\n", i, deviceName(dev.Get()).c_str());
            }
        }
    }
    std::printf("\n");

    // --- 2. Default render device ---
    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAIL_IF(FAILED(hr), "GetDefaultAudioEndpoint", hr)) return failures;
    std::printf("Default output: %s\n\n", deviceName(device.Get()).c_str());

    // --- 3. Render client ---
    ComPtr<IAudioClient> renderAC;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &renderAC);
    if (FAIL_IF(FAILED(hr), "Activate renderAC", hr)) return failures;

    WAVEFORMATEX* rawFmt = nullptr;
    hr = renderAC->GetMixFormat(&rawFmt);
    if (FAIL_IF(FAILED(hr), "GetMixFormat", hr)) return failures;
    WaveFormatPtr mixFmt(rawFmt);

    std::printf("Mix format : %u Hz, %u ch, %u-bit  (float=%s)\n\n",
                mixFmt->nSamplesPerSec, mixFmt->nChannels, mixFmt->wBitsPerSample,
                isMixFormatFloat(mixFmt.get()) ? "yes" : "no");

    if (FAIL_IF(!isMixFormatFloat(mixFmt.get()), "Mix format must be IEEE float")) return failures;

    constexpr REFERENCE_TIME kBufDuration = 2000000; // 200ms in 100ns units

    hr = renderAC->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                              kBufDuration, 0, mixFmt.get(), nullptr);
    if (FAIL_IF(FAILED(hr), "renderAC->Initialize", hr)) return failures;

    UINT32 renderBufFrames = 0;
    renderAC->GetBufferSize(&renderBufFrames);

    ComPtr<IAudioRenderClient> renderSvc;
    hr = renderAC->GetService(IID_PPV_ARGS(&renderSvc));
    if (FAIL_IF(FAILED(hr), "GetService(IAudioRenderClient)", hr)) return failures;

    // --- 4. Loopback capture client (same render device, LOOPBACK flag) ---
    ComPtr<IAudioClient> captureAC;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &captureAC);
    if (FAIL_IF(FAILED(hr), "Activate captureAC", hr)) return failures;

    hr = captureAC->Initialize(AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_LOOPBACK,
                               kBufDuration, 0, mixFmt.get(), nullptr);
    if (FAIL_IF(FAILED(hr), "captureAC->Initialize (loopback)", hr)) return failures;

    ComPtr<IAudioCaptureClient> captureSvc;
    hr = captureAC->GetService(IID_PPV_ARGS(&captureSvc));
    if (FAIL_IF(FAILED(hr), "GetService(IAudioCaptureClient)", hr)) return failures;

    // --- 5. Pre-fill render buffer with silence, then start both ---
    {
        BYTE* buf = nullptr;
        renderSvc->GetBuffer(renderBufFrames, &buf);
        renderSvc->ReleaseBuffer(renderBufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    captureAC->Start();
    renderAC->Start();

    // --- 6. Run for 3 seconds: render 1 kHz sine, capture loopback ---
    std::printf("Rendering 1 kHz sine + capturing loopback for 3 seconds...\n");

    const UINT32 sampleRate = mixFmt->nSamplesPerSec;
    const UINT32 channels   = mixFmt->nChannels;
    const float  amplitude  = 0.15f;
    const double phaseInc   = 2.0 * PI * 1000.0 / sampleRate;
    double phase = 0.0;

    std::vector<float> captured;
    captured.reserve(sampleRate * 3);

    const int kIterations = 300; // 300 × 10ms = 3 seconds
    for (int i = 0; i < kIterations; ++i) {
        // Render: fill however much space is available
        UINT32 padding = 0;
        renderAC->GetCurrentPadding(&padding);
        UINT32 avail = renderBufFrames - padding;
        if (avail > 0) {
            BYTE* buf = nullptr;
            if (SUCCEEDED(renderSvc->GetBuffer(avail, &buf))) {
                auto* s = reinterpret_cast<float*>(buf);
                for (UINT32 f = 0; f < avail; ++f) {
                    float sample = amplitude * static_cast<float>(std::sin(phase));
                    phase += phaseInc;
                    for (UINT32 c = 0; c < channels; ++c)
                        s[f * channels + c] = sample;
                }
                renderSvc->ReleaseBuffer(avail, 0);
            }
        }

        // Capture: drain all available loopback packets
        UINT32 pktSize = 0;
        while (SUCCEEDED(captureSvc->GetNextPacketSize(&pktSize)) && pktSize > 0) {
            BYTE* buf = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (SUCCEEDED(captureSvc->GetBuffer(&buf, &frames, &flags, nullptr, nullptr))) {
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    auto* s = reinterpret_cast<float*>(buf);
                    for (UINT32 f = 0; f < frames; ++f)
                        captured.push_back(s[f * channels]); // ch0 only
                }
                captureSvc->ReleaseBuffer(frames);
            }
        }

        Sleep(10);
    }

    renderAC->Stop();
    captureAC->Stop();

    // --- 7. Analyse ---
    double rms          = computeRMS(captured);
    double expectedRms  = amplitude / std::sqrt(2.0); // ~0.106
    std::printf("Captured frames : %zu\n", captured.size());
    std::printf("Captured RMS    : %.4f\n", rms);
    std::printf("Expected RMS    : ~%.4f  (sine amp=%.2f, 100%% vol)\n\n", expectedRms, amplitude);

    // Assertions
    {
        char d[128];
        std::snprintf(d, sizeof(d), "(%zu frames)", captured.size());
        FAIL_IF(captured.size() < sampleRate * 2,
                (std::string("too few frames captured  ") + d).c_str());

        std::snprintf(d, sizeof(d), "(%.4f — check master volume > 0)", rms);
        FAIL_IF(rms < 0.002,
                (std::string("captured RMS near zero  ") + d).c_str());
    }

    std::printf("---\n");
    if (failures == 0) std::printf("ALL TESTS PASSED\n");
    else               std::printf("%d TEST(S) FAILED\n", failures);

    return failures;
}

// ---------------------------------------------------------------------------
int main()
{
    std::printf("AnniAudio — Phase 0 POC: WASAPI loopback capture + render\n\n");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { std::fprintf(stderr, "CoInitializeEx failed\n"); return 1; }
    int result = run();
    CoUninitialize();
    return result == 0 ? 0 : 1;
}
