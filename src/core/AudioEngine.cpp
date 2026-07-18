#include "AudioEngine.hpp"

#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

// Local definitions for KS audio format subtypes; avoids linking against
// ksuser/ksguid for the standard KSDATAFORMAT_SUBTYPE_* GUIDs.
static const GUID KSCONST_SUBTYPE_IEEE_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID KSCONST_SUBTYPE_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

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

// ---------------------------------------------------------------------------
// Format helpers
// ---------------------------------------------------------------------------

static bool isFloatWfx(const WAVEFORMATEX* wfx)
{
    if (!wfx) return false;
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        auto wfex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        return IsEqualGUID(wfex->SubFormat, KSCONST_SUBTYPE_IEEE_FLOAT);
    }
    return false;
}

static bool isPcm16Wfx(const WAVEFORMATEX* wfx)
{
    if (!wfx) return false;
    if (wfx->wFormatTag == WAVE_FORMAT_PCM) return wfx->wBitsPerSample == 16;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        auto wfex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        return IsEqualGUID(wfex->SubFormat, KSCONST_SUBTYPE_PCM) && wfex->Format.wBitsPerSample == 16;
    }
    return false;
}

static WfxPtr makeFloatFormat(const WAVEFORMATEX* mix)
{
    if (!mix) return nullptr;
    auto* wfex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    if (!wfex) return nullptr;

    WORD ch = mix->nChannels;
    DWORD rate = mix->nSamplesPerSec;
    WORD bytes = ch * sizeof(float);

    wfex->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wfex->Format.nChannels       = ch;
    wfex->Format.nSamplesPerSec  = rate;
    wfex->Format.nAvgBytesPerSec = rate * bytes;
    wfex->Format.nBlockAlign     = bytes;
    wfex->Format.wBitsPerSample  = 32;
    wfex->Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfex->Samples.wValidBitsPerSample = 32;
    wfex->dwChannelMask          = (ch == 1) ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    wfex->SubFormat              = KSCONST_SUBTYPE_IEEE_FLOAT;

    return WfxPtr(reinterpret_cast<WAVEFORMATEX*>(wfex));
}

static WfxPtr tryForceFloat(IAudioClient* client, const WAVEFORMATEX* mix)
{
    WfxPtr floatFmt = makeFloatFormat(mix);
    if (!floatFmt || !client) return nullptr;

    WAVEFORMATEX* closest = nullptr;
    HRESULT hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, floatFmt.get(), &closest);

    if (hr == S_OK) {
        return floatFmt;
    }
    if (hr == S_FALSE && closest && isFloatWfx(closest)) {
        return WfxPtr(closest);
    }
    if (closest) CoTaskMemFree(closest);
    return nullptr;
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

// Find a device by name across both render and capture endpoints.
// If the hint matches both, prefer capture (safer default); callers that want
// loopback from a render device should pass an unambiguous render-only name.
static ComPtr<IMMDevice> findAnyDevice(IMMDeviceEnumerator* enumerator,
                                        const std::string& hint,
                                        EDataFlow* foundFlow)
{
    ComPtr<IMMDevice> renderDev;
    ComPtr<IMMDevice> captureDev;
    EDataFlow flows[2] = { eRender, eCapture };
    for (EDataFlow flow : flows) {
        ComPtr<IMMDeviceCollection> col;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) continue;
        UINT count = 0; col->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            if (SUCCEEDED(col->Item(i, &dev)) && nameContains(friendlyName(dev.Get()), hint)) {
                if (flow == eRender) renderDev = dev;
                else captureDev = dev;
                break; // first match in this direction
            }
        }
    }
    if (captureDev && renderDev) {
        std::fprintf(stderr, "[Engine] Warning: name '%s' matches both render and capture endpoints. Using capture.\n", hint.c_str());
        *foundFlow = eCapture;
        return captureDev;
    }
    if (captureDev) { *foundFlow = eCapture; return captureDev; }
    if (renderDev) { *foundFlow = eRender; return renderDev; }
    *foundFlow = eAll;
    return nullptr;
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

static void pcm16ToFloat(const BYTE* src, size_t sampleCount, float* dst)
{
    auto* p = reinterpret_cast<const int16_t*>(src);
    const float scale = 1.0f / 32768.0f;
    for (size_t i = 0; i < sampleCount; ++i) dst[i] = p[i] * scale;
}

static void floatToPcm16(const float* src, size_t sampleCount, BYTE* dst)
{
    auto* p = reinterpret_cast<int16_t*>(dst);
    for (size_t i = 0; i < sampleCount; ++i) {
        float s = src[i];
        if (s > 1.0f) s = 1.0f;
        else if (s < -1.0f) s = -1.0f;
        p[i] = static_cast<int16_t>(s * 32767.0f);
    }
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
    // True when capture format != render format (rate or channels)
    bool needsConvert{false};
    // Sample format flags
    bool renderIsFloat{true};
    bool captureIsFloat{true};
    bool captureIsLoopback{false};
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
    std::atomic<float>    volume{1.0f};

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
    const bool     rFloat = renderIsFloat;
    const bool     cFloat = captureIsFloat;
    const double   ratio  = (double)renderRate / (double)captureRate; // dstRate/srcRate
    // worst-case output frames for one capture packet (allow 3x for upsampling)
    const size_t   cvtMax = (size_t)(renderBufFrames * 3);
    std::vector<float> captureTmp;  // holds raw capture packet in float
    captureTmp.reserve(renderBufFrames * cCh * 2);
    std::vector<float> renderTmp;   // temp float buffer when render is PCM16
    renderTmp.reserve(renderBufFrames * rCh);

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
                    size_t sampleCount = (size_t)frames * cCh;
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        // Push equivalent silence in render format
                        auto dstFrames = static_cast<uint32_t>(std::ceil(frames * ratio));
                        convertBuf.assign((size_t)dstFrames * rCh, 0.0f);
                        ring.write(convertBuf.data(), (size_t)dstFrames * rCh);
                    } else {
                        captureTmp.resize(sampleCount);
                        if (cFloat) {
                            std::copy(reinterpret_cast<const float*>(data),
                                      reinterpret_cast<const float*>(data) + sampleCount,
                                      captureTmp.data());
                        } else {
                            pcm16ToFloat(data, sampleCount, captureTmp.data());
                        }
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
                    size_t sampleCount = (size_t)toWrite * rCh;
                    float vol = volume.load(std::memory_order_relaxed);
                    if (rFloat) {
                        auto* fbuf = reinterpret_cast<float*>(buf);
                        ring.readOrSilence(fbuf, sampleCount);
                        if (vol != 1.0f) {
                            for (size_t i = 0; i < sampleCount; ++i) fbuf[i] *= vol;
                        }
                    } else {
                        renderTmp.resize(sampleCount);
                        ring.readOrSilence(renderTmp.data(), sampleCount);
                        if (vol != 1.0f) {
                            for (auto& s : renderTmp) s *= vol;
                        }
                        floatToPcm16(renderTmp.data(), sampleCount, buf);
                    }
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
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return out;

    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                            CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) {
        out = enumEndpoints(enumerator.Get());
    }
    CoUninitialize();
    return out;
}

bool AudioEngine::start(const std::string& captureHint, const std::string& renderHint)
{
    if (m_impl->running) stop();

    // Initialize COM for this thread (idempotent; runThread re-initializes on its own thread)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) { return false; }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, IID_PPV_ARGS(&m_impl->enumerator));
    if (FAILED(hr)) { std::fprintf(stderr, "[Engine] CoCreateInstance failed 0x%08X\n", (unsigned)hr); return false; }

    // --- Render device ---
    auto renderDev = findDevice(m_impl->enumerator.Get(), eRender, renderHint);
    if (!renderDev) { std::fprintf(stderr, "[Engine] Render device not found: \"%s\"\n", renderHint.c_str()); return false; }
    std::fprintf(stderr, "[Engine] Render  : %s\n", friendlyName(renderDev.Get()).c_str());

    hr = renderDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &m_impl->renderAC);
    if (FAILED(hr)) return false;

    {
        WAVEFORMATEX* raw = nullptr;
        hr = m_impl->renderAC->GetMixFormat(&raw);
        if (FAILED(hr) || !raw) { std::fprintf(stderr, "[Engine] renderAC->GetMixFormat failed 0x%08X\n", (unsigned)hr); m_impl->cleanup(); return false; }
        m_impl->renderFmt.reset(raw);
        // Try to use a 32-bit float shared format if the endpoint supports it (the audio engine
        // generally prefers this and it avoids per-sample conversion on our side).
        WfxPtr forced = tryForceFloat(m_impl->renderAC.Get(), m_impl->renderFmt.get());
        if (forced) m_impl->renderFmt = std::move(forced);
    }
    if (!m_impl->renderFmt) { m_impl->cleanup(); return false; }
    m_impl->renderCh   = m_impl->renderFmt->nChannels;
    m_impl->renderRate = m_impl->renderFmt->nSamplesPerSec;
    m_impl->renderIsFloat = isFloatWfx(m_impl->renderFmt.get());
    std::fprintf(stderr, "[Engine] Render  format : %u Hz, %u ch, %u-bit (%s)\n",
                 m_impl->renderRate, m_impl->renderCh, m_impl->renderFmt->wBitsPerSample,
                 m_impl->renderIsFloat ? "float" : "pcm");

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
    // If the hint resolves to a render endpoint, use WASAPI loopback capture.
    EDataFlow captureFlow = eAll;
    auto captureDev = findAnyDevice(m_impl->enumerator.Get(), captureHint, &captureFlow);
    if (!captureDev) { std::fprintf(stderr, "[Engine] Capture device not found: \"%s\"\n", captureHint.c_str()); m_impl->cleanup(); return false; }
    m_impl->captureIsLoopback = (captureFlow == eRender);
    std::fprintf(stderr, "[Engine] Capture : %s%s\n",
                 friendlyName(captureDev.Get()).c_str(),
                 m_impl->captureIsLoopback ? " [loopback]" : "");

    hr = captureDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &m_impl->captureAC);
    if (FAILED(hr)) { m_impl->cleanup(); return false; }

    {
        WAVEFORMATEX* raw = nullptr;
        hr = m_impl->captureAC->GetMixFormat(&raw);
        if (FAILED(hr) || !raw) { std::fprintf(stderr, "[Engine] captureAC->GetMixFormat failed 0x%08X\n", (unsigned)hr); m_impl->cleanup(); return false; }
        m_impl->captureFmt.reset(raw);
        WfxPtr forced = tryForceFloat(m_impl->captureAC.Get(), m_impl->captureFmt.get());
        if (forced) m_impl->captureFmt = std::move(forced);
    }
    if (!m_impl->captureFmt) { m_impl->cleanup(); return false; }
    m_impl->captureCh   = m_impl->captureFmt->nChannels;
    m_impl->captureRate = m_impl->captureFmt->nSamplesPerSec;
    m_impl->captureIsFloat = isFloatWfx(m_impl->captureFmt.get());
    std::fprintf(stderr, "[Engine] Capture format : %u Hz, %u ch, %u-bit (%s)%s\n",
                 m_impl->captureRate, m_impl->captureCh, m_impl->captureFmt->wBitsPerSample,
                 m_impl->captureIsFloat ? "float" : "pcm",
                 m_impl->captureIsLoopback ? " [loopback]" : "");

    m_impl->needsConvert = (m_impl->captureCh   != m_impl->renderCh ||
                            m_impl->captureRate  != m_impl->renderRate);
    if (m_impl->needsConvert)
        std::fprintf(stderr, "[Engine] Format conversion active (%.0f→%.0f Hz, %u→%u ch)\n",
                     (double)m_impl->captureRate, (double)m_impl->renderRate,
                     m_impl->captureCh, m_impl->renderCh);

    DWORD captureFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (m_impl->captureIsLoopback) captureFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    hr = m_impl->captureAC->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        captureFlags,
                                        kBuf, 0, m_impl->captureFmt.get(), nullptr);
    if (FAILED(hr)) { std::fprintf(stderr, "[Engine] captureAC->Initialize failed 0x%08X\n", (unsigned)hr); m_impl->cleanup(); return false; }
    m_impl->captureAC->SetEventHandle(m_impl->captureEvent);
    hr = m_impl->captureAC->GetService(IID_PPV_ARGS(&m_impl->captureSvc));
    if (FAILED(hr)) { m_impl->cleanup(); return false; }

    m_impl->srcPhase = 0.0;
    // Ring in render format: several buffer durations of headroom
    m_impl->ring.init((size_t)m_impl->renderBufFrames * 4 * m_impl->renderCh);
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

void AudioEngine::setVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;  // allow modest gain; clamp to avoid accidental overload
    m_impl->volume.store(v, std::memory_order_relaxed);
}

float AudioEngine::getVolume() const
{
    return m_impl->volume.load(std::memory_order_relaxed);
}
