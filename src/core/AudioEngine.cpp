#include "AudioEngine.hpp"
#include "audio_utils.hpp"

#include <windows.h>
#include <avrt.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace anniaudio::core;

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
    // Pre-allocated audio-thread buffers (no heap allocations on the hot path)
    std::vector<float> captureTmp;
    std::vector<float> convertBuf;
    std::vector<float> renderTmp;

    HANDLE stopEvent{nullptr}, captureEvent{nullptr}, renderEvent{nullptr};
    HANDLE thread{nullptr};

    UINT32 renderBufFrames{0};
    UINT32 captureBufFrames{0};
    UINT32 cvtMax{0};          // max render frames for one capture buffer

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
                        std::fill_n(convertBuf.data(), (size_t)dstFrames * rCh, 0.0f);
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
                            uint32_t dstFrames = convertBuffer(
                                captureTmp.data(), frames, cCh,
                                convertBuf.data(), cvtMax, rCh,
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
    m_impl->captureAC->GetBufferSize(&m_impl->captureBufFrames);
    hr = m_impl->captureAC->GetService(IID_PPV_ARGS(&m_impl->captureSvc));
    if (FAILED(hr)) { m_impl->cleanup(); return false; }

    m_impl->srcPhase = 0.0;

    // Pre-allocate audio-thread buffers so the hot path never hits the heap.
    {
        const double ratio = (double)m_impl->renderRate / (double)m_impl->captureRate;
        const double upRatio = std::max(ratio, 1.0);
        m_impl->cvtMax = (uint32_t)std::ceil((double)m_impl->captureBufFrames * upRatio) + m_impl->renderCh;

        m_impl->captureTmp.resize((size_t)m_impl->captureBufFrames * m_impl->captureCh);
        m_impl->convertBuf.resize((size_t)m_impl->cvtMax * m_impl->renderCh);
        m_impl->renderTmp.resize((size_t)m_impl->renderBufFrames * m_impl->renderCh);
    }

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

bool     AudioEngine::isRunning()           const { return m_impl->running.load(); }
uint32_t AudioEngine::sampleRate()          const { return m_impl->renderRate; }
uint32_t AudioEngine::channelCount()        const { return m_impl->renderCh; }
uint32_t AudioEngine::captureSampleRate() const { return m_impl->captureRate; }
uint32_t AudioEngine::captureChannelCount() const { return m_impl->captureCh; }
uint64_t AudioEngine::framesProcessed()     const { return m_impl->framesProcessed.load(); }

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
