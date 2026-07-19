#include "AudioMixer.hpp"
#include "audio_utils.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>
#include <avrt.h>

namespace anniaudio::core {

using Microsoft::WRL::ComPtr;

constexpr UINT64 kDefaultBuffer100Ns = 2000000; // 200 ms shared buffer

struct AudioMixer::Impl {
    struct Strip {
        std::string name;
        ComPtr<IAudioClient>        captureAC;
        ComPtr<IAudioCaptureClient> captureSvc;
        HANDLE captureEvent = nullptr;
        WfxPtr captureFmt;

        uint32_t captureCh   = 0;
        uint32_t captureRate = 0;
        bool     captureIsFloat = true;
        bool     captureIsLoopback = false;

        bool     needsConvert = false;
        double   ratio = 1.0;     // renderRate / captureRate
        double   srcPhase = 0.0;
        uint32_t cvtMax = 0;

        std::vector<float> captureTmp;
        std::vector<float> convertBuf;
        std::vector<float> readBuf;
        RingBuffer ring;

        std::atomic<float> volume{1.0f};
        std::atomic<bool>  muted{false};
    };

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice>           renderDev;
    ComPtr<IAudioClient>        renderAC;
    ComPtr<IAudioRenderClient>  renderSvc;
    HANDLE                      renderEvent = nullptr;
    WfxPtr                      renderFmt;

    uint32_t renderCh   = 0;
    uint32_t renderRate = 0;
    bool     renderIsFloat = true;
    UINT32   renderBufFrames = 0;

    std::vector<std::unique_ptr<Strip>> strips;
    std::string outputName;
    std::vector<float> mixBuf;

    HANDLE stopEvent = nullptr;
    HANDLE thread    = nullptr;
    std::atomic<bool> running{false};
    std::atomic<float> masterVolume{1.0f};

    static DWORD WINAPI threadEntry(LPVOID p) { reinterpret_cast<Impl*>(p)->run(); return 0; }

    bool openOutput(const std::string& outputHint);
    bool openStrip(Strip& s, const std::string& sourceHint);

    void run();
    void processRender();
    void processStrip(size_t idx);
    void cleanup();
};

bool AudioMixer::Impl::openOutput(const std::string& outputHint)
{
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) return false;

    renderDev = findDevice(enumerator.Get(), eRender, outputHint);
    if (!renderDev) {
        std::fprintf(stderr, "[mixer] Could not find render output '%s'\n", outputHint.c_str());
        return false;
    }

    wchar_t* id = nullptr;
    renderDev->GetId(&id);
    if (id) { CoTaskMemFree(id); }
    outputName = friendlyName(renderDev.Get());

    hr = renderDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(renderAC.GetAddressOf()));
    if (FAILED(hr) || !renderAC) return false;

    WAVEFORMATEX* raw = nullptr;
    hr = renderAC->GetMixFormat(&raw);
    if (FAILED(hr) || !raw) return false;
    renderFmt.reset(raw);

    WfxPtr forced = tryForceFloat(renderAC.Get(), renderFmt.get());
    if (forced) renderFmt = std::move(forced);

    renderCh      = renderFmt->nChannels;
    renderRate    = renderFmt->nSamplesPerSec;
    renderIsFloat = isFloatWfx(renderFmt.get());

    hr = renderAC->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                              kDefaultBuffer100Ns, 0, renderFmt.get(), nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[mixer] renderAC->Initialize failed 0x%08X\n", (unsigned)hr);
        return false;
    }

    renderEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!renderEvent) return false;
    renderAC->SetEventHandle(renderEvent);

    hr = renderAC->GetService(IID_PPV_ARGS(&renderSvc));
    if (FAILED(hr) || !renderSvc) return false;

    renderAC->GetBufferSize(&renderBufFrames);
    if (renderBufFrames == 0) renderBufFrames = renderRate / 100; // fallback ~10ms

    std::fprintf(stderr, "[mixer] Output  : %s\n", outputName.c_str());
    std::fprintf(stderr, "[mixer] Output  format : %u Hz, %u ch, %s\n",
                 renderRate, renderCh, renderIsFloat ? "float" : "pcm");
    return true;
}

bool AudioMixer::Impl::openStrip(Strip& s, const std::string& sourceHint)
{
    EDataFlow foundFlow = eAll;
    ComPtr<IMMDevice> dev = findAnyDevice(enumerator.Get(), sourceHint, &foundFlow);
    if (!dev) {
        std::fprintf(stderr, "[mixer] Could not find source '%s'\n", sourceHint.c_str());
        return false;
    }
    s.captureIsLoopback = (foundFlow == eRender);

    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(s.captureAC.GetAddressOf()));
    if (FAILED(hr) || !s.captureAC) return false;

    WAVEFORMATEX* raw = nullptr;
    hr = s.captureAC->GetMixFormat(&raw);
    if (FAILED(hr) || !raw) return false;
    s.captureFmt.reset(raw);

    WfxPtr forced = tryForceFloat(s.captureAC.Get(), s.captureFmt.get());
    if (forced) s.captureFmt = std::move(forced);

    s.captureCh      = s.captureFmt->nChannels;
    s.captureRate    = s.captureFmt->nSamplesPerSec;
    s.captureIsFloat = isFloatWfx(s.captureFmt.get());

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (s.captureIsLoopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    hr = s.captureAC->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                 kDefaultBuffer100Ns, 0, s.captureFmt.get(), nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[mixer] captureAC->Initialize for '%s' failed 0x%08X\n",
                     s.name.empty() ? sourceHint.c_str() : s.name.c_str(), (unsigned)hr);
        return false;
    }

    s.captureEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!s.captureEvent) return false;
    s.captureAC->SetEventHandle(s.captureEvent);

    hr = s.captureAC->GetService(IID_PPV_ARGS(&s.captureSvc));
    if (FAILED(hr) || !s.captureSvc) return false;

    UINT32 bufFrames = 0;
    s.captureAC->GetBufferSize(&bufFrames);
    if (bufFrames == 0) bufFrames = s.captureRate / 100;

    s.needsConvert = (s.captureCh != renderCh) || (s.captureRate != renderRate);
    s.ratio        = (double)renderRate / (double)s.captureRate;
    s.srcPhase     = 0.0;

    const double upRatio = std::max(s.ratio, 1.0);
    s.cvtMax = (uint32_t)std::ceil((double)bufFrames * upRatio) + renderCh;

    s.captureTmp.resize((size_t)bufFrames * s.captureCh);
    s.convertBuf.resize((size_t)s.cvtMax * renderCh);
    s.readBuf.resize((size_t)renderBufFrames * renderCh);
    s.ring.init((size_t)renderBufFrames * renderCh * 4);

    std::fprintf(stderr, "[mixer] Strip %zu : %s%s\n", strips.size(),
                 s.name.c_str(),
                 s.captureIsLoopback ? " [loopback]" : "");
    std::fprintf(stderr, "[mixer]  format : %u Hz, %u ch, %s\n",
                 s.captureRate, s.captureCh, s.captureIsFloat ? "float" : "pcm");
    if (s.needsConvert) {
        std::fprintf(stderr, "[mixer]  conversion active (%.0f→%.0f Hz, %u→%u ch)\n",
                     (double)s.captureRate, (double)renderRate, s.captureCh, renderCh);
    }
    return true;
}

void AudioMixer::Impl::run()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD taskIdx = 0;
    HANDLE mmTask = AvSetMmThreadCharacteristics(L"Audio", &taskIdx);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    std::vector<HANDLE> handles;
    handles.reserve(2 + strips.size());
    handles.push_back(stopEvent);
    handles.push_back(renderEvent);
    for (auto& s : strips) handles.push_back(s->captureEvent);

    for (auto& s : strips) s->captureAC->Start();
    renderAC->Start();
    running = true;

    while (true) {
        DWORD w = WaitForMultipleObjects((DWORD)handles.size(), handles.data(), FALSE, 200);
        if (w == WAIT_OBJECT_0) break;

        size_t idx = (w >= WAIT_OBJECT_0) ? (w - WAIT_OBJECT_0) : (size_t)-1;
        if (idx == 1) {
            processRender();
        } else if (idx >= 2 && idx < handles.size()) {
            processStrip(idx - 2);
        }
    }

    for (auto& s : strips) if (s->captureAC) s->captureAC->Stop();
    if (renderAC) renderAC->Stop();
    if (mmTask) AvRevertMmThreadCharacteristics(mmTask);
    CoUninitialize();
    running = false;
}

void AudioMixer::Impl::processStrip(size_t idx)
{
    if (idx >= strips.size()) return;
    Strip& s = *strips[idx];

    UINT32 pktSize = 0;
    while (SUCCEEDED(s.captureSvc->GetNextPacketSize(&pktSize)) && pktSize > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(s.captureSvc->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        if (frames == 0) { s.captureSvc->ReleaseBuffer(frames); continue; }

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            uint32_t dstFrames = (uint32_t)std::ceil((double)frames * s.ratio);
            std::fill_n(s.convertBuf.data(), (size_t)dstFrames * renderCh, 0.0f);
            s.ring.write(s.convertBuf.data(), (size_t)dstFrames * renderCh);
        } else {
            size_t sampleCount = (size_t)frames * s.captureCh;
            if (s.captureTmp.size() < sampleCount) s.captureTmp.resize(sampleCount);
            if (s.captureIsFloat) {
                std::copy(reinterpret_cast<const float*>(data),
                          reinterpret_cast<const float*>(data) + sampleCount,
                          s.captureTmp.data());
            } else {
                pcm16ToFloat(data, sampleCount, s.captureTmp.data());
            }

            if (s.needsConvert) {
                uint32_t dstFrames = convertBuffer(
                    s.captureTmp.data(), frames, s.captureCh,
                    s.convertBuf.data(), s.cvtMax, renderCh,
                    s.ratio, s.srcPhase);
                s.ring.write(s.convertBuf.data(), (size_t)dstFrames * renderCh);
            } else {
                s.ring.write(s.captureTmp.data(), sampleCount);
            }
        }
        s.captureSvc->ReleaseBuffer(frames);
    }
}

void AudioMixer::Impl::processRender()
{
    UINT32 padding = 0;
    renderAC->GetCurrentPadding(&padding);
    UINT32 toWrite = renderBufFrames - padding;
    if (toWrite == 0) return;

    BYTE* buf = nullptr;
    if (FAILED(renderSvc->GetBuffer(toWrite, &buf))) return;

    const size_t outSamples = (size_t)toWrite * renderCh;

    // Clear mix buffer
    if (mixBuf.size() < outSamples) mixBuf.resize(outSamples);
    std::fill_n(mixBuf.data(), outSamples, 0.0f);

    for (auto& s : strips) {
        float vol = s->muted.load() ? 0.0f : s->volume.load();
        if (vol == 0.0f) {
            // Still consume so the strip doesn't drift/fill up.
            s->ring.readOrSilence(s->readBuf.data(), outSamples);
            continue;
        }
        s->ring.readOrSilence(s->readBuf.data(), outSamples);
        const float* src = s->readBuf.data();
        float* dst = mixBuf.data();
        for (size_t i = 0; i < outSamples; ++i) dst[i] += src[i] * vol;
    }

    float master = masterVolume.load();
    if (master != 1.0f) {
        for (size_t i = 0; i < outSamples; ++i) mixBuf[i] *= master;
    }

    if (renderIsFloat) {
        auto* fbuf = reinterpret_cast<float*>(buf);
        for (size_t i = 0; i < outSamples; ++i) {
            float v = mixBuf[i];
            if (v >  1.0f) v =  1.0f;
            if (v < -1.0f) v = -1.0f;
            fbuf[i] = v;
        }
    } else {
        // floatToPcm16 clamps each sample itself.
        floatToPcm16(mixBuf.data(), outSamples, buf);
    }

    renderSvc->ReleaseBuffer(toWrite, 0);
}

void AudioMixer::Impl::cleanup()
{
    if (stopEvent) { CloseHandle(stopEvent); stopEvent = nullptr; }
    if (renderEvent) { CloseHandle(renderEvent); renderEvent = nullptr; }
    if (thread) { CloseHandle(thread); thread = nullptr; }
    for (auto& s : strips) {
        if (s->captureEvent) { CloseHandle(s->captureEvent); s->captureEvent = nullptr; }
    }
    renderSvc.Reset();
    renderAC.Reset();
    renderDev.Reset();
    for (auto& s : strips) {
        s->captureSvc.Reset();
        s->captureAC.Reset();
    }
    strips.clear();
    running = false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AudioMixer::AudioMixer() : m_impl(std::make_unique<Impl>()) {}
AudioMixer::~AudioMixer() { stop(); }

bool AudioMixer::init(const std::string& outputHint)
{
    stop();
    m_impl->stopEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    return m_impl->openOutput(outputHint);
}

int AudioMixer::addStrip(const MixerStripConfig& cfg)
{
    if (!m_impl->renderAC) return -1;
    if (m_impl->strips.size() >= 62) {
        std::fprintf(stderr, "[mixer] Too many strips (max 62)\n");
        return -1;
    }

    auto s = std::make_unique<Impl::Strip>();
    s->name   = cfg.name.empty() ? cfg.source : cfg.name;
    s->volume.store(cfg.volume);
    s->muted.store(cfg.muted);

    if (!m_impl->openStrip(*s, cfg.source)) return -1;

    int idx = static_cast<int>(m_impl->strips.size());
    m_impl->strips.push_back(std::move(s));
    return idx;
}

bool AudioMixer::start()
{
    if (!m_impl->renderAC || m_impl->strips.empty()) return false;
    m_impl->thread = CreateThread(nullptr, 0, Impl::threadEntry, m_impl.get(), 0, nullptr);
    if (!m_impl->thread) return false;

    // Wait briefly for the thread to start its clients
    for (int i = 0; i < 50 && !m_impl->running; ++i) Sleep(10);
    return m_impl->running;
}

void AudioMixer::stop()
{
    if (!m_impl->stopEvent) return;
    SetEvent(m_impl->stopEvent);
    if (m_impl->thread) {
        WaitForSingleObject(m_impl->thread, 5000);
        CloseHandle(m_impl->thread);
        m_impl->thread = nullptr;
    }
    m_impl->cleanup();
}

bool AudioMixer::running() const noexcept { return m_impl->running.load(); }

size_t AudioMixer::stripCount() const noexcept { return m_impl->strips.size(); }

void AudioMixer::setStripVolume(size_t idx, float vol)
{
    if (idx >= m_impl->strips.size()) return;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 2.0f) vol = 2.0f;
    m_impl->strips[idx]->volume.store(vol);
}

float AudioMixer::stripVolume(size_t idx) const
{
    if (idx >= m_impl->strips.size()) return 0.0f;
    return m_impl->strips[idx]->volume.load();
}

void AudioMixer::setStripMuted(size_t idx, bool mute)
{
    if (idx >= m_impl->strips.size()) return;
    m_impl->strips[idx]->muted.store(mute);
}

bool AudioMixer::stripMuted(size_t idx) const
{
    if (idx >= m_impl->strips.size()) return true;
    return m_impl->strips[idx]->muted.load();
}

void AudioMixer::setMasterVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    m_impl->masterVolume.store(v);
}

float AudioMixer::masterVolume() const { return m_impl->masterVolume.load(); }

const std::string& AudioMixer::outputName() const { return m_impl->outputName; }

const std::string& AudioMixer::stripName(size_t idx) const
{
    static const std::string empty;
    if (idx >= m_impl->strips.size()) return empty;
    return m_impl->strips[idx]->name;
}

} // namespace anniaudio::core
