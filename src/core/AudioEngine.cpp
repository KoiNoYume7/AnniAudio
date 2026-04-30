#include "AudioEngine.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct WfxDeleter { void operator()(WAVEFORMATEX* p) { CoTaskMemFree(p); } };
using WfxPtr = std::unique_ptr<WAVEFORMATEX, WfxDeleter>;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::string wideToUtf8(const wchar_t* w)
{
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::string friendlyName(IMMDevice* dev)
{
    ComPtr<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return {};
    PROPVARIANT pv; PropVariantInit(&pv);
    props->GetValue(PKEY_Device_FriendlyName, &pv);
    std::string name = (pv.vt == VT_LPWSTR) ? wideToUtf8(pv.pwszVal) : std::string{};
    PropVariantClear(&pv);
    return name;
}

static bool nameContains(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
        [](char a, char b){ return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); })
        != hay.end();
}

static ComPtr<IMMDevice> findDevice(IMMDeviceEnumerator* enumerator,
                                     EDataFlow flow, const std::string& hint)
{
    ComPtr<IMMDevice> result;
    if (hint.empty()) { enumerator->GetDefaultAudioEndpoint(flow, eConsole, &result); return result; }
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return result;
    UINT count = 0; col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (SUCCEEDED(col->Item(i, &dev)) && nameContains(friendlyName(dev.Get()), hint)) {
            result = dev; break;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Enumerate all active endpoints into EndpointInfo structs
// ---------------------------------------------------------------------------

static std::vector<EndpointInfo> enumEndpoints(IMMDeviceEnumerator* enumerator)
{
    std::vector<EndpointInfo> out;
    if (!enumerator) return out;

    // Determine defaults
    ComPtr<IMMDevice> defRender, defCapture;
    enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defRender);
    enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &defCapture);

    wchar_t* defRenderId = nullptr; wchar_t* defCaptureId = nullptr;
    if (defRender) defRender->GetId(&defRenderId);
    if (defCapture) defCapture->GetId(&defCaptureId);
    std::string defRenderStr = defRenderId ? wideToUtf8(defRenderId) : "";
    std::string defCaptureStr = defCaptureId ? wideToUtf8(defCaptureId) : "";
    if (defRenderId) CoTaskMemFree(defRenderId);
    if (defCaptureId) CoTaskMemFree(defCaptureId);

    for (int pass = 0; pass < 2; ++pass) {
        EDataFlow flow = (pass == 0) ? eRender : eCapture;
        ComPtr<IMMDeviceCollection> col;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) continue;
        UINT count = 0; col->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            if (FAILED(col->Item(i, &dev))) continue;
            wchar_t* id = nullptr; dev->GetId(&id);
            std::string idStr = id ? wideToUtf8(id) : "";
            if (id) CoTaskMemFree(id);
            std::string name = friendlyName(dev.Get());
            bool isDefault = (flow == eRender)
                ? (idStr == defRenderStr)
                : (idStr == defCaptureStr);
            out.push_back({ idStr, name, flow == eRender,
                            nameContains(name, "AnniAudio"), isDefault });
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Format conversion: captureFormat → renderFormat
// Handles channel count mismatch and sample rate mismatch (linear interp SRC).
// srcPhase: persistent accumulator (initialise to 0, pass by ref between calls).
// Returns number of render frames written to dst.
// ---------------------------------------------------------------------------

static uint32_t convertBuffer(
    const float* src, uint32_t srcFrames, uint32_t srcCh,
    float*       dst, uint32_t dstMaxFrames, uint32_t dstCh,
    double srcToDstRatio,   // dstRate / srcRate
    double& phase)          // fractional position into src (state)
{
    uint32_t dstFrame = 0;
    while (dstFrame < dstMaxFrames) {
        // integer + fractional index into src
        auto   idx0  = static_cast<uint32_t>(phase);
        double frac  = phase - idx0;
        uint32_t idx1 = std::min(idx0 + 1, srcFrames - 1);

        if (idx0 >= srcFrames) break; // exhausted src

        // Write one dst frame (linear interpolation per channel)
        uint32_t chsToCopy = std::min(srcCh, dstCh);
        for (uint32_t c = 0; c < chsToCopy; ++c) {
            float s0 = src[idx0 * srcCh + c];
            float s1 = src[idx1 * srcCh + c];
            dst[dstFrame * dstCh + c] = s0 + static_cast<float>(frac) * (s1 - s0);
        }
        // Zero any extra dst channels
        for (uint32_t c = chsToCopy; c < dstCh; ++c)
            dst[dstFrame * dstCh + c] = 0.0f;

        phase += 1.0 / srcToDstRatio; // advance src position by one dst step
        ++dstFrame;
    }
    // Carry over fractional remainder relative to end of this src block
    phase = std::max(0.0, phase - srcFrames);
    return dstFrame;
}

// ---------------------------------------------------------------------------
// Simple single-producer/single-consumer ring buffer (float samples)
// ---------------------------------------------------------------------------

class RingBuffer {
public:
    void init(size_t capacity) {
        m_buf.assign(capacity, 0.0f);
        m_head = m_tail = 0; m_cap = capacity;
    }
    size_t available() const {
        return (m_head >= m_tail) ? m_head - m_tail : m_cap - (m_tail - m_head);
    }
    void write(const float* src, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            m_buf[m_head] = src[i];
            m_head = (m_head + 1) % m_cap;
            if (m_head == m_tail) m_tail = (m_tail + 1) % m_cap; // drop oldest on overflow
        }
    }
    void readOrSilence(float* dst, size_t n) {
        size_t got = std::min(n, available());
        for (size_t i = 0; i < got;  ++i) { dst[i] = m_buf[m_tail]; m_tail = (m_tail + 1) % m_cap; }
        for (size_t i = got; i < n;  ++i) dst[i] = 0.0f;
    }
private:
    std::vector<float> m_buf;
    size_t m_head{0}, m_tail{0}, m_cap{0};
};

// ---------------------------------------------------------------------------
// PIMPL
// ---------------------------------------------------------------------------

struct AudioEngine::Impl {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IAudioClient>        captureAC, renderAC;
    ComPtr<IAudioCaptureClient> captureSvc;
    ComPtr<IAudioRenderClient>  renderSvc;

    // Each device uses its own native mix format.
    WfxPtr renderFmt;   // drives the render endpoint and ring buffer layout
    WfxPtr captureFmt;  // native format of the capture endpoint

    // Render (canonical) format details
    uint32_t renderCh{0}, renderRate{0};
    // Capture format details
    uint32_t captureCh{0}, captureRate{0};
    // True when capture format != render format
    bool needsConvert{false};
    // SRC phase accumulator (persists between audio thread calls)
    double srcPhase{0.0};
    // Temp buffer for format conversion output (render format)
    std::vector<float> convertBuf;

    HANDLE stopEvent{nullptr}, captureEvent{nullptr}, renderEvent{nullptr};
    HANDLE thread{nullptr};

    UINT32 renderBufFrames{0};

    RingBuffer ring;
    ProcessFn  processFn;

    std::atomic<uint64_t> framesProcessed{0};
    std::atomic<bool>     running{false};

    static DWORD WINAPI threadEntry(LPVOID p) { reinterpret_cast<Impl*>(p)->runThread(); return 0; }
    void runThread();
    void cleanup();
};

void AudioEngine::Impl::runThread()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD taskIdx = 0;
    HANDLE mmTask = AvSetMmThreadCharacteristics(L"Audio", &taskIdx);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    const uint32_t rCh   = renderCh;
    const uint32_t cCh   = captureCh;
    const double   ratio  = (double)renderRate / (double)captureRate; // dstRate/srcRate
    // worst-case output frames for one capture packet (allow 3x for upsampling)
    const size_t   cvtMax = (size_t)(renderBufFrames * 3);
    std::vector<float> captureTmp;  // holds raw capture packet (capture format)
    captureTmp.reserve(renderBufFrames * cCh * 2);

    captureAC->Start();
    renderAC->Start();
    running = true;

    HANDLE ev[3] = { stopEvent, captureEvent, renderEvent };

    while (true) {
        DWORD w = WaitForMultipleObjects(3, ev, FALSE, 200);
        if (w == WAIT_OBJECT_0) break;

        if (w == WAIT_OBJECT_0 + 1) {
            // Capture event: drain all available packets → convert → ring
            UINT32 pktSize = 0;
            while (SUCCEEDED(captureSvc->GetNextPacketSize(&pktSize)) && pktSize > 0) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                if (SUCCEEDED(captureSvc->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        // Push equivalent silence in render format
                        auto dstFrames = static_cast<uint32_t>(std::ceil(frames * ratio));
                        convertBuf.assign((size_t)dstFrames * rCh, 0.0f);
                        ring.write(convertBuf.data(), (size_t)dstFrames * rCh);
                    } else {
                        captureTmp.resize((size_t)frames * cCh);
                        std::copy(reinterpret_cast<const float*>(data),
                                  reinterpret_cast<const float*>(data) + (size_t)frames * cCh,
                                  captureTmp.data());
                        if (processFn) processFn(captureTmp.data(), frames, cCh);

                        if (needsConvert) {
                            convertBuf.resize(cvtMax * rCh);
                            uint32_t dstFrames = convertBuffer(
                                captureTmp.data(), frames, cCh,
                                convertBuf.data(), static_cast<uint32_t>(cvtMax), rCh,
                                ratio, srcPhase);
                            ring.write(convertBuf.data(), (size_t)dstFrames * rCh);
                        } else {
                            ring.write(captureTmp.data(), (size_t)frames * rCh);
                        }
                    }
                    captureSvc->ReleaseBuffer(frames);
                    framesProcessed.fetch_add(frames, std::memory_order_relaxed);
                }
            }
        }

        if (w == WAIT_OBJECT_0 + 2) {
            // Render event: ring → render buffer
            UINT32 padding = 0;
            renderAC->GetCurrentPadding(&padding);
            UINT32 toWrite = renderBufFrames - padding;
            if (toWrite > 0) {
                BYTE* buf = nullptr;
                if (SUCCEEDED(renderSvc->GetBuffer(toWrite, &buf))) {
                    ring.readOrSilence(reinterpret_cast<float*>(buf), (size_t)toWrite * rCh);
                    renderSvc->ReleaseBuffer(toWrite, 0);
                }
            }
        }
    }

    renderAC->Stop();
    captureAC->Stop();
    if (mmTask) AvRevertMmThreadCharacteristics(mmTask);
    CoUninitialize();
    running = false;
}

void AudioEngine::Impl::cleanup()
{
    if (stopEvent)    { CloseHandle(stopEvent);    stopEvent    = nullptr; }
    if (captureEvent) { CloseHandle(captureEvent); captureEvent = nullptr; }
    if (renderEvent)  { CloseHandle(renderEvent);  renderEvent  = nullptr; }
    if (thread)       { CloseHandle(thread);       thread       = nullptr; }
    captureAC.Reset(); renderAC.Reset(); captureSvc.Reset(); renderSvc.Reset();
    enumerator.Reset(); renderFmt.reset(); captureFmt.reset();
    running = false;
}

// ---------------------------------------------------------------------------
// AudioEngine public API
// ---------------------------------------------------------------------------

AudioEngine::AudioEngine()  : m_impl(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() { stop(); }

void AudioEngine::setProcessCallback(ProcessFn fn) { m_impl->processFn = std::move(fn); }

std::vector<EndpointInfo> AudioEngine::listEndpoints() const
{
    std::vector<EndpointInfo> out;
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return out;
    return enumEndpoints(enumerator.Get());
}

bool AudioEngine::start(const std::string& captureHint, const std::string& renderHint)
{
    if (m_impl->running) stop();

    HRESULT hr = S_OK;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, IID_PPV_ARGS(&m_impl->enumerator));
    if (FAILED(hr)) { std::fprintf(stderr, "[Engine] CoCreateInstance failed 0x%08X\n", (unsigned)hr); return false; }

    // --- Render device ---
    auto renderDev = findDevice(m_impl->enumerator.Get(), eRender, renderHint);
    if (!renderDev) { std::fprintf(stderr, "[Engine] Render device not found: \"%s\"\n", renderHint.c_str()); return false; }
    std::fprintf(stderr, "[Engine] Render  : %s\n", friendlyName(renderDev.Get()).c_str());

    hr = renderDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &m_impl->renderAC);
    if (FAILED(hr)) return false;

    { WAVEFORMATEX* raw = nullptr; m_impl->renderAC->GetMixFormat(&raw); m_impl->renderFmt.reset(raw); }
    m_impl->renderCh   = m_impl->renderFmt->nChannels;
    m_impl->renderRate = m_impl->renderFmt->nSamplesPerSec;
    std::fprintf(stderr, "[Engine] Render  format : %u Hz, %u ch, %u-bit\n",
                 m_impl->renderRate, m_impl->renderCh, m_impl->renderFmt->wBitsPerSample);

    constexpr REFERENCE_TIME kBuf = 2000000; // 200ms
    m_impl->stopEvent    = CreateEvent(nullptr, TRUE,  FALSE, nullptr);
    m_impl->captureEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    m_impl->renderEvent  = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    hr = m_impl->renderAC->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                       kBuf, 0, m_impl->renderFmt.get(), nullptr);
    if (FAILED(hr)) { std::fprintf(stderr, "[Engine] renderAC->Initialize failed 0x%08X\n", (unsigned)hr); m_impl->cleanup(); return false; }
    m_impl->renderAC->SetEventHandle(m_impl->renderEvent);
    m_impl->renderAC->GetBufferSize(&m_impl->renderBufFrames);
    hr = m_impl->renderAC->GetService(IID_PPV_ARGS(&m_impl->renderSvc));
    if (FAILED(hr)) { m_impl->cleanup(); return false; }

    { BYTE* b = nullptr; m_impl->renderSvc->GetBuffer(m_impl->renderBufFrames, &b);
      m_impl->renderSvc->ReleaseBuffer(m_impl->renderBufFrames, AUDCLNT_BUFFERFLAGS_SILENT); }

    // --- Capture device (opened with its OWN native mix format) ---
    auto captureDev = findDevice(m_impl->enumerator.Get(), eCapture, captureHint);
    if (!captureDev) { std::fprintf(stderr, "[Engine] Capture device not found: \"%s\"\n", captureHint.c_str()); m_impl->cleanup(); return false; }
    std::fprintf(stderr, "[Engine] Capture : %s\n", friendlyName(captureDev.Get()).c_str());

    hr = captureDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &m_impl->captureAC);
    if (FAILED(hr)) { m_impl->cleanup(); return false; }

    { WAVEFORMATEX* raw = nullptr; m_impl->captureAC->GetMixFormat(&raw); m_impl->captureFmt.reset(raw); }
    m_impl->captureCh   = m_impl->captureFmt->nChannels;
    m_impl->captureRate = m_impl->captureFmt->nSamplesPerSec;
    std::fprintf(stderr, "[Engine] Capture format : %u Hz, %u ch, %u-bit\n",
                 m_impl->captureRate, m_impl->captureCh, m_impl->captureFmt->wBitsPerSample);

    m_impl->needsConvert = (m_impl->captureCh   != m_impl->renderCh ||
                            m_impl->captureRate  != m_impl->renderRate);
    if (m_impl->needsConvert)
        std::fprintf(stderr, "[Engine] Format conversion active (%.0f→%.0f Hz, %u→%u ch)\n",
                     (double)m_impl->captureRate, (double)m_impl->renderRate,
                     m_impl->captureCh, m_impl->renderCh);

    hr = m_impl->captureAC->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        kBuf, 0, m_impl->captureFmt.get(), nullptr);
    if (FAILED(hr)) { std::fprintf(stderr, "[Engine] captureAC->Initialize failed 0x%08X\n", (unsigned)hr); m_impl->cleanup(); return false; }
    m_impl->captureAC->SetEventHandle(m_impl->captureEvent);
    hr = m_impl->captureAC->GetService(IID_PPV_ARGS(&m_impl->captureSvc));
    if (FAILED(hr)) { m_impl->cleanup(); return false; }

    m_impl->srcPhase = 0.0;
    // Ring in render format: 8192 render frames (~85ms at 96kHz)
    m_impl->ring.init((size_t)8192 * m_impl->renderCh);
    m_impl->framesProcessed = 0;

    m_impl->thread = CreateThread(nullptr, 0, Impl::threadEntry, m_impl.get(), 0, nullptr);
    if (!m_impl->thread) { m_impl->cleanup(); return false; }

    // Wait for thread to confirm audio clients are started
    for (int i = 0; i < 50 && !m_impl->running; ++i) Sleep(10);
    return true;
}

void AudioEngine::stop()
{
    if (!m_impl->stopEvent) return;
    SetEvent(m_impl->stopEvent);
    if (m_impl->thread) WaitForSingleObject(m_impl->thread, 5000);
    m_impl->cleanup();
}

bool     AudioEngine::isRunning()       const { return m_impl->running.load(); }
uint32_t AudioEngine::sampleRate()      const { return m_impl->renderRate; }
uint32_t AudioEngine::channelCount()    const { return m_impl->renderCh; }
uint64_t AudioEngine::framesProcessed() const { return m_impl->framesProcessed.load(); }
