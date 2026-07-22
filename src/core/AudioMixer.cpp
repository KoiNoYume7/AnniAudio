#include "AudioMixer.hpp"
#include "audio_utils.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <avrt.h>
#include <audioclientactivationparams.h>
#include <objidl.h>

#include "eq.hpp"
#include "noise_suppressor.hpp"
#include "spatializer.hpp"

#pragma comment(lib, "mmdevapi.lib")

namespace anniaudio::core {

using Microsoft::WRL::ComPtr;

constexpr UINT64 kDefaultBuffer100Ns = 2000000; // 200 ms shared buffer

// WaitForMultipleObjects caps out at MAXIMUM_WAIT_OBJECTS (64). Two slots are
// reserved for stopEvent and renderEvent, leaving this many for strips.
constexpr size_t kMaxStrips = 62;

// ---------------------------------------------------------------------------
// Impl
//
// Locking model
// --------------
// `strips` (the ordered list mixed each render tick) has exactly one writer:
// the audio thread itself, and only while holding `structureMutex`. It is
// mutated in two situations:
//   1. Before start() has ever been called: there is no audio thread yet, so
//      the calling thread (still under `structureMutex`, for uniformity) can
//      write directly.
//   2. While running: any thread wanting to add/remove/rename a strip
//      prepares the change (which may fail — e.g. device not found — and can
//      take a little time, e.g. opening a WASAPI client) OFF the audio
//      thread, then pushes a ready-to-apply `Command` onto a small
//      mutex-protected queue. The audio thread drains that queue and applies
//      each command — a `strips` vector insert/erase, nothing that can block
//      or fail — once per iteration of its own loop, under `structureMutex`.
//
// Because the audio thread is the only writer, and it only ever writes while
// holding the lock, its own *unlocked* reads of `strips` later in the same
// loop iteration (the actual per-sample WASAPI/ring-buffer work) are safe:
// nothing else can be concurrently mutating the vector out from under it.
// Other threads reading `strips` (snapshot(), resolving a StripId to a
// Strip for a volume/mute write) take `structureMutex` just long enough to
// copy out a `shared_ptr<Strip>`, then release it — so they never hold the
// lock while touching a Strip's contents, and never block the audio thread
// for anything more than a vector insert/erase/scan.
//
// Per-strip volume/mute remain plain atomics on the Strip object (unchanged
// from the original design) — once you have a shared_ptr<Strip>, writing
// those is lock-free and instantaneous, no queue involved.
// ---------------------------------------------------------------------------

struct AudioMixer::Impl {
    struct Strip {
        StripId     id = 0;
        std::string name;
        std::string source;
        std::optional<int> knobIndex;

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

        // Capture-side DSP (denoise, EQ). Allocated and prepared in
        // setupStripDsp() before the strip reaches the audio thread; only
        // process() is called from there, which is allocation-free.
        std::unique_ptr<anniaudio::dsp::NoiseSuppressor> denoiser;
        std::unique_ptr<anniaudio::dsp::EqChain> eq;

        // HRTF spatializer (mono -> positioned stereo). When present it replaces
        // the strip's normal channel-convert step: the strip is downmixed to
        // mono, resampled to the render rate, then convolved to stereo. Prepared
        // in setupStripDsp() before the audio thread; only process()/setDirection()
        // run on the audio thread, both allocation-free.
        std::unique_ptr<anniaudio::dsp::Spatializer> spatializer;
        std::vector<float> monoTmp;    // downmixed source, capture rate
        std::vector<float> monoRes;    // downmixed source, render rate
        std::vector<float> stereoTmp;  // spatialized interleaved stereo
        // Requested direction (written by any thread via setStripDirection), and
        // the last direction actually applied (audio thread only).
        std::atomic<float> reqAz{0.0f};
        std::atomic<float> reqEl{0.0f};
        std::atomic<bool>  dirDirty{false};
        float appliedAz = 0.0f;
        float appliedEl = 0.0f;

        std::atomic<float> volume{1.0f};
        std::atomic<bool>  muted{false};

        // Post-fader levels, written by the audio thread and read by the API.
        std::atomic<float> peak{0.0f};
        std::atomic<float> rms{0.0f};

        // Set once by the audio thread when it Start()s the capture client
        // (either at mixer start, or when applying a live AddStrip command),
        // so teardown knows whether Stop() is meaningful to call.
        bool started = false;
    };

    struct Command {
        enum class Kind { Add, Remove, Rename, SetKnobIndex } kind;
        std::shared_ptr<Strip> strip;   // Add only
        StripId             targetId = 0; // Remove, Rename, SetKnobIndex
        std::string          text;        // Rename: new name
        std::optional<int>   knobIndex;    // SetKnobIndex
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

    // Guards `strips`' shape (see class-level comment). Never held during
    // blocking WASAPI I/O.
    mutable std::mutex structureMutex;
    std::vector<std::shared_ptr<Strip>> strips;

    std::mutex commandMutex;
    std::vector<Command> pendingCommands;
    std::atomic<uint32_t> nextStripId{1};

    std::string outputName;
    std::vector<float> mixBuf;

    HANDLE stopEvent = nullptr;
    HANDLE thread    = nullptr;
    std::atomic<bool> running{false};
    std::atomic<float> masterVolume{1.0f};
    std::atomic<bool>  masterMuted{false};
    std::atomic<float> masterPeak{0.0f};
    std::atomic<float> masterRms{0.0f};

    static DWORD WINAPI threadEntry(LPVOID p) { reinterpret_cast<Impl*>(p)->run(); return 0; }

    bool openOutput(const std::string& outputHint);
    bool openStrip(Strip& s, const MixerStripConfig& cfg);
    bool openApplicationLoopback(Strip& s, uint32_t pid);
    bool openEndpointStrip(Strip& s, const std::string& sourceHint);
    bool finalizeStripBuffers(Strip& s, uint32_t bufFrames);
    void setupStripDsp(Strip& s, const MixerStripConfig& cfg);
    void enqueue(Command cmd);

    void run();
    void applyPendingCommands(bool& handlesDirty);
    void processRender();
    void processStrip(Strip& s);
    void writeStripSpatial(Strip& s, uint32_t frames); // mono in s.monoTmp -> stereo ring
    void teardownStrip(Strip& s);
    void cleanup();

    std::shared_ptr<Strip> findStripLocked(StripId id) const; // caller holds structureMutex
    std::shared_ptr<Strip> findStrip(StripId id) const;        // takes the lock itself

    StripSnapshot toSnapshot(const Strip& s) const;
};

std::shared_ptr<AudioMixer::Impl::Strip> AudioMixer::Impl::findStripLocked(StripId id) const
{
    for (auto& s : strips) {
        if (s->id == id) return s;
    }
    return nullptr;
}

std::shared_ptr<AudioMixer::Impl::Strip> AudioMixer::Impl::findStrip(StripId id) const
{
    std::lock_guard<std::mutex> lk(structureMutex);
    return findStripLocked(id);
}

StripSnapshot AudioMixer::Impl::toSnapshot(const Strip& s) const
{
    StripSnapshot snap;
    snap.id        = s.id;
    snap.name      = s.name;
    snap.source    = s.source;
    snap.output    = outputName;
    snap.volume    = s.volume.load();
    snap.muted     = s.muted.load();
    snap.knobIndex = s.knobIndex;
    snap.spatial   = (bool)s.spatializer;
    snap.azimuth   = s.reqAz.load();
    snap.elevation = s.reqEl.load();
    snap.peak      = s.peak.load();
    snap.rms       = s.rms.load();
    return snap;
}

void AudioMixer::Impl::enqueue(Command cmd)
{
    std::lock_guard<std::mutex> lk(commandMutex);
    pendingCommands.push_back(std::move(cmd));
}

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

bool AudioMixer::Impl::openStrip(Strip& s, const MixerStripConfig& cfg)
{
    bool ok = false;
    if (cfg.sourceType == StripSourceType::Application) {
        try {
            size_t pos = 0;
            uint32_t pid = static_cast<uint32_t>(std::stoull(cfg.source, &pos));
            if (pos == 0 || pos != cfg.source.size() || pid == 0) {
                std::fprintf(stderr, "[mixer] Invalid application process id '%s'\n", cfg.source.c_str());
                return false;
            }
            ok = openApplicationLoopback(s, pid);
        } catch (const std::exception&) {
            std::fprintf(stderr, "[mixer] Invalid application process id '%s'\n", cfg.source.c_str());
            return false;
        }
    } else {
        ok = openEndpointStrip(s, cfg.source);
    }
    if (!ok) return false;
    setupStripDsp(s, cfg);
    return true;
}

// Locate the bundled HRTF dataset. The mixer normally runs with the repo root
// as its working directory (start-mixer.bat), but fall back to a path resolved
// from the executable location so it also works when launched from elsewhere.
static std::string resolveHrtfPath()
{
    auto exists = [](const std::string& p) {
        DWORD a = GetFileAttributesA(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    };
    const char* rel = "assets/hrtf/mit_kemar.sofa";
    if (exists(rel)) return rel;

    char exePath[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
        std::string dir(exePath);
        size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) dir.resize(slash);
        // exe lives in build/bin/Release — walk up to the repo root.
        for (const char* up : {"/assets/hrtf/mit_kemar.sofa",
                               "/../../../assets/hrtf/mit_kemar.sofa"}) {
            std::string cand = dir + up;
            if (exists(cand)) return cand;
        }
    }
    return rel; // best effort; loadHrtf() will log if it can't open it
}

void AudioMixer::Impl::setupStripDsp(Strip& s, const MixerStripConfig& cfg)
{
    if (cfg.denoise) {
        // RNNoise is trained on 48 kHz audio; other rates would need an extra
        // resample stage, so for now the flag is only honored at 48 kHz.
        if (s.captureRate == 48000) {
            s.denoiser = std::make_unique<anniaudio::dsp::NoiseSuppressor>();
            s.denoiser->prepare(s.captureCh);
            if (!s.denoiser->prepared()) {
                std::fprintf(stderr, "[mixer] denoise init failed for '%s'\n", s.name.c_str());
                s.denoiser.reset();
            } else {
                std::fprintf(stderr, "[mixer] denoise enabled for '%s' (%u ch)\n", s.name.c_str(), s.captureCh);
            }
        } else {
            std::fprintf(stderr, "[mixer] denoise skipped for '%s': capture rate %u != 48000\n",
                         s.name.c_str(), s.captureRate);
        }
    }
    if (cfg.eqPreset == "voice") {
        s.eq = std::make_unique<anniaudio::dsp::EqChain>();
        s.eq->addBand(anniaudio::dsp::FilterType::HighPass, 80.0, 0.0, 0.707);    // rumble
        s.eq->addBand(anniaudio::dsp::FilterType::Peak, 250.0, -2.0, 1.0);        // mud
        s.eq->addBand(anniaudio::dsp::FilterType::Peak, 3000.0, 2.5, 1.0);        // presence
        s.eq->addBand(anniaudio::dsp::FilterType::HighShelf, 8000.0, 1.5, 0.707); // air
        s.eq->prepare(s.captureRate, s.captureCh);
        std::fprintf(stderr, "[mixer] voice EQ enabled for '%s'\n", s.name.c_str());
    } else if (!cfg.eqPreset.empty()) {
        std::fprintf(stderr, "[mixer] unknown eq preset '%s' for '%s' (available: voice)\n",
                     cfg.eqPreset.c_str(), s.name.c_str());
    }

    if (cfg.spatial) {
        if (renderCh < 2) {
            std::fprintf(stderr, "[mixer] spatial skipped for '%s': output '%s' has %u channel(s), needs >= 2\n",
                         s.name.c_str(), outputName.c_str(), renderCh);
        } else {
            // maxBlock must cover the largest resampled mono block a packet can
            // produce; cvtMax is exactly that bound (see finalizeStripBuffers).
            const uint32_t maxBlock = s.cvtMax;
            s.spatializer = std::make_unique<anniaudio::dsp::Spatializer>();
            if (s.spatializer->loadHrtf(resolveHrtfPath(), (double)renderRate, maxBlock)) {
                s.spatializer->setDirection(cfg.azimuth, cfg.elevation);
                s.appliedAz = cfg.azimuth;
                s.appliedEl = cfg.elevation;
                s.reqAz.store(cfg.azimuth);
                s.reqEl.store(cfg.elevation);
                s.monoTmp.resize(s.captureTmp.size() / (s.captureCh ? s.captureCh : 1) + 1);
                s.monoRes.resize(s.cvtMax + 1);
                s.stereoTmp.resize((size_t)s.cvtMax * 2 + 2);
                std::fprintf(stderr, "[mixer] spatial enabled for '%s' (az=%.0f el=%.0f, %u taps @ %u Hz)\n",
                             s.name.c_str(), cfg.azimuth, cfg.elevation,
                             s.spatializer->irLength(), renderRate);
            } else {
                std::fprintf(stderr, "[mixer] spatial init failed for '%s' (HRTF load)\n", s.name.c_str());
                s.spatializer.reset();
            }
        }
    }
}

bool AudioMixer::Impl::openEndpointStrip(Strip& s, const std::string& sourceHint)
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

    return finalizeStripBuffers(s, bufFrames);
}

bool AudioMixer::Impl::finalizeStripBuffers(Strip& s, uint32_t bufFrames)
{
    s.needsConvert = (s.captureCh != renderCh) || (s.captureRate != renderRate);
    s.ratio        = (double)renderRate / (double)s.captureRate;
    s.srcPhase     = 0.0;

    const double upRatio = std::max(s.ratio, 1.0);
    s.cvtMax = (uint32_t)std::ceil((double)bufFrames * upRatio) + renderCh;

    s.captureTmp.resize((size_t)bufFrames * s.captureCh);
    s.convertBuf.resize((size_t)s.cvtMax * renderCh);
    s.readBuf.resize((size_t)renderBufFrames * renderCh);
    s.ring.init((size_t)renderBufFrames * renderCh * 4);

    std::fprintf(stderr, "[mixer] Strip %u : %s%s\n", s.id, s.name.c_str(),
                 s.captureIsLoopback ? " [loopback]" : "");
    std::fprintf(stderr, "[mixer]  format : %u Hz, %u ch, %s\n",
                 s.captureRate, s.captureCh, s.captureIsFloat ? "float" : "pcm");
    if (s.needsConvert) {
        std::fprintf(stderr, "[mixer]  conversion active (%.0f→%.0f Hz, %u→%u ch)\n",
                     (double)s.captureRate, (double)renderRate, s.captureCh, renderCh);
    }
    return true;
}

class ProcessLoopbackActivationHandler :
    public IActivateAudioInterfaceCompletionHandler,
    public IAgileObject {
public:
    ProcessLoopbackActivationHandler() : m_ref(1), m_event(CreateEvent(nullptr, FALSE, FALSE, nullptr)) {}
    ~ProcessLoopbackActivationHandler() { if (m_event) CloseHandle(m_event); }

    HANDLE eventHandle() const { return m_event; }
    HRESULT result() const { return m_result; }
    IAudioClient* client() const { return m_client.Get(); }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, __uuidof(IActivateAudioInterfaceCompletionHandler))) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (IsEqualIID(riid, IID_IAgileObject)) {
            *ppv = static_cast<IAgileObject*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&m_ref); }
    STDMETHOD_(ULONG, Release)() override {
        ULONG c = InterlockedDecrement(&m_ref);
        if (c == 0) delete this;
        return c;
    }

    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override {
        m_result = E_FAIL;
        if (!operation) {
            SetEvent(m_event);
            return S_OK;
        }
        HRESULT hrActivate = E_UNEXPECTED;
        ComPtr<IUnknown> punk;
        HRESULT hr = operation->GetActivateResult(&hrActivate, &punk);
        if (SUCCEEDED(hr) && SUCCEEDED(hrActivate) && punk) {
            punk.As(&m_client);
            m_result = m_client ? S_OK : E_NOINTERFACE;
        } else {
            m_result = FAILED(hrActivate) ? hrActivate : hr;
        }
        SetEvent(m_event);
        return S_OK;
    }

private:
    volatile LONG m_ref;
    HANDLE m_event;
    HRESULT m_result = E_FAIL;
    ComPtr<IAudioClient> m_client;
};

static bool initProcessLoopbackFormat(WAVEFORMATEXTENSIBLE& wfex)
{
    wfex.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wfex.Format.nChannels       = 2;
    wfex.Format.nSamplesPerSec  = 48000;
    wfex.Format.wBitsPerSample  = 32;
    wfex.Format.nBlockAlign     = wfex.Format.nChannels * sizeof(float);
    wfex.Format.nAvgBytesPerSec = wfex.Format.nSamplesPerSec * wfex.Format.nBlockAlign;
    wfex.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfex.Samples.wValidBitsPerSample = 32;
    wfex.dwChannelMask          = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wfex.SubFormat              = KSCONST_SUBTYPE_IEEE_FLOAT;
    return true;
}

bool AudioMixer::Impl::openApplicationLoopback(Strip& s, uint32_t pid)
{
    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    params.ProcessLoopbackParams.TargetProcessId = pid;

    PROPVARIANT activateParams = {};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    // The handler starts with a refcount of 1, so Attach (rather than the
    // AddRef'ing constructor) keeps the count balanced and lets ComPtr delete
    // it once the async operation has released its own reference.
    ComPtr<ProcessLoopbackActivationHandler> handler;
    handler.Attach(new ProcessLoopbackActivationHandler());

    ComPtr<IActivateAudioInterfaceAsyncOperation> asyncOp;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activateParams,
        handler.Get(),
        &asyncOp);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[mixer] ActivateAudioInterfaceAsync failed for pid %u: 0x%08X\n", pid, (unsigned)hr);
        return false;
    }

    DWORD wait = WaitForSingleObject(handler->eventHandle(), 10000);
    if (wait != WAIT_OBJECT_0) {
        std::fprintf(stderr, "[mixer] Timeout waiting for process loopback activation (pid %u)\n", pid);
        return false;
    }
    hr = handler->result();
    if (FAILED(hr) || !handler->client()) {
        std::fprintf(stderr, "[mixer] Process loopback activation failed for pid %u: 0x%08X\n", pid, (unsigned)hr);
        return false;
    }
    s.captureAC = handler->client();

    WAVEFORMATEXTENSIBLE wfex;
    initProcessLoopbackFormat(wfex);

    DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
    hr = s.captureAC->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0,
                                 reinterpret_cast<const WAVEFORMATEX*>(&wfex), nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[mixer] Process loopback IAudioClient::Initialize failed for pid %u: 0x%08X\n", pid, (unsigned)hr);
        s.captureAC.Reset();
        return false;
    }

    hr = s.captureAC->SetEventHandle(s.captureEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[mixer] SetEventHandle failed for pid %u: 0x%08X\n", pid, (unsigned)hr);
        return false;
    }

    hr = s.captureAC->GetService(IID_PPV_ARGS(&s.captureSvc));
    if (FAILED(hr) || !s.captureSvc) {
        std::fprintf(stderr, "[mixer] GetService(IAudioCaptureClient) failed for pid %u: 0x%08X\n", pid, (unsigned)hr);
        return false;
    }

    UINT32 bufFrames = 0;
    s.captureAC->GetBufferSize(&bufFrames);
    if (bufFrames == 0) bufFrames = wfex.Format.nSamplesPerSec / 100;

    s.captureCh      = wfex.Format.nChannels;
    s.captureRate    = wfex.Format.nSamplesPerSec;
    s.captureIsFloat = true;
    s.captureIsLoopback = true;

    return finalizeStripBuffers(s, bufFrames);
}

void AudioMixer::Impl::teardownStrip(Strip& s)
{
    if (s.started && s.captureAC) s.captureAC->Stop();
    if (s.captureEvent) { CloseHandle(s.captureEvent); s.captureEvent = nullptr; }
    s.captureSvc.Reset();
    s.captureAC.Reset();
    s.denoiser.reset();
    s.eq.reset();
    s.spatializer.reset();
}

void AudioMixer::Impl::applyPendingCommands(bool& handlesDirty)
{
    std::vector<Command> local;
    {
        std::lock_guard<std::mutex> lk(commandMutex);
        if (pendingCommands.empty()) return;
        local.swap(pendingCommands);
    }

    std::lock_guard<std::mutex> lk(structureMutex);
    for (auto& cmd : local) {
        switch (cmd.kind) {
        case Command::Kind::Add: {
            if (strips.size() >= kMaxStrips) {
                std::fprintf(stderr, "[mixer] Dropping live AddStrip for '%s': strip limit (%zu) reached\n",
                             cmd.strip->name.c_str(), kMaxStrips);
                break;
            }
            cmd.strip->captureAC->Start();
            cmd.strip->started = true;
            strips.push_back(cmd.strip);
            handlesDirty = true;
            std::fprintf(stderr, "[mixer] Added strip %u : %s\n", cmd.strip->id, cmd.strip->name.c_str());
            break;
        }
        case Command::Kind::Remove: {
            auto it = std::find_if(strips.begin(), strips.end(),
                [&](const std::shared_ptr<Strip>& s) { return s->id == cmd.targetId; });
            if (it == strips.end()) break;
            std::fprintf(stderr, "[mixer] Removed strip %u : %s\n", (*it)->id, (*it)->name.c_str());
            teardownStrip(**it);
            strips.erase(it);
            handlesDirty = true;
            break;
        }
        case Command::Kind::Rename: {
            if (auto s = findStripLocked(cmd.targetId)) s->name = cmd.text;
            break;
        }
        case Command::Kind::SetKnobIndex: {
            if (auto s = findStripLocked(cmd.targetId)) s->knobIndex = cmd.knobIndex;
            break;
        }
        }
    }
}

void AudioMixer::Impl::run()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD taskIdx = 0;
    HANDLE mmTask = AvSetMmThreadCharacteristics(L"Audio", &taskIdx);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    for (auto& s : strips) { s->captureAC->Start(); s->started = true; }
    renderAC->Start();
    running = true;

    std::vector<HANDLE> handles;
    bool handlesDirty = true;

    while (true) {
        bool dirty = false;
        applyPendingCommands(dirty);
        if (dirty) handlesDirty = true;

        if (handlesDirty) {
            handles.clear();
            handles.reserve(2 + strips.size());
            handles.push_back(stopEvent);
            handles.push_back(renderEvent);
            for (auto& s : strips) handles.push_back(s->captureEvent);
            handlesDirty = false;
        }

        DWORD w = WaitForMultipleObjects((DWORD)handles.size(), handles.data(), FALSE, 200);
        if (w == WAIT_OBJECT_0) break;

        size_t idx = (w >= WAIT_OBJECT_0) ? (w - WAIT_OBJECT_0) : (size_t)-1;
        if (idx == 1) {
            processRender();
        } else if (idx >= 2 && idx < handles.size()) {
            size_t stripIdx = idx - 2;
            if (stripIdx < strips.size()) processStrip(*strips[stripIdx]);
        }
    }

    for (auto& s : strips) teardownStrip(*s);
    if (renderAC) renderAC->Stop();
    if (mmTask) AvRevertMmThreadCharacteristics(mmTask);
    CoUninitialize();
    running = false;
}

void AudioMixer::Impl::processStrip(Strip& s)
{
    UINT32 pktSize = 0;
    while (SUCCEEDED(s.captureSvc->GetNextPacketSize(&pktSize)) && pktSize > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(s.captureSvc->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        if (frames == 0) { s.captureSvc->ReleaseBuffer(frames); continue; }

        // Apply a pending live direction change on the audio thread, serialized
        // with process() (both run here), so the HRIR swap can't race.
        if (s.spatializer && s.dirDirty.exchange(false)) {
            float az = s.reqAz.load(), el = s.reqEl.load();
            if (az != s.appliedAz || el != s.appliedEl) {
                s.spatializer->setDirection(az, el);
                s.appliedAz = az;
                s.appliedEl = el;
            }
        }

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            if (s.spatializer) {
                // Feed silence through so the mono resample phase and the
                // spatializer's overlap tail stay coherent across gaps.
                if (s.monoTmp.size() < frames) s.monoTmp.resize(frames);
                std::fill_n(s.monoTmp.data(), frames, 0.0f);
                writeStripSpatial(s, frames);
            } else {
                uint32_t dstFrames = (uint32_t)std::ceil((double)frames * s.ratio);
                std::fill_n(s.convertBuf.data(), (size_t)dstFrames * renderCh, 0.0f);
                s.ring.write(s.convertBuf.data(), (size_t)dstFrames * renderCh);
            }
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

            // Capture-side DSP at the source's native rate, before any
            // resampling, so every route of this strip gets the clean signal.
            if (s.denoiser) s.denoiser->process(s.captureTmp.data(), frames, s.captureCh);
            if (s.eq)       s.eq->process(s.captureTmp.data(), frames, s.captureCh);

            if (s.spatializer) {
                // Downmix to mono at the capture rate, then hand off to the
                // spatial path (resample -> convolve -> stereo).
                if (s.monoTmp.size() < frames) s.monoTmp.resize(frames);
                const float inv = 1.0f / (float)s.captureCh;
                for (uint32_t f = 0; f < frames; ++f) {
                    float acc = 0.0f;
                    const float* fr = &s.captureTmp[(size_t)f * s.captureCh];
                    for (uint32_t c = 0; c < s.captureCh; ++c) acc += fr[c];
                    s.monoTmp[f] = acc * inv;
                }
                writeStripSpatial(s, frames);
            } else if (s.needsConvert) {
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

// Takes `frames` mono samples (capture rate) already sitting in s.monoTmp,
// resamples them to the render rate, convolves to a positioned stereo image,
// and writes the result into the strip's ring as renderCh-interleaved audio.
void AudioMixer::Impl::writeStripSpatial(Strip& s, uint32_t frames)
{
    const float* monoSrc = s.monoTmp.data();
    uint32_t outFrames = frames;

    if (s.captureRate != renderRate) {
        if (s.monoRes.size() < s.cvtMax) s.monoRes.resize(s.cvtMax);
        outFrames = convertBuffer(s.monoTmp.data(), frames, 1,
                                  s.monoRes.data(), s.cvtMax, 1,
                                  s.ratio, s.srcPhase);
        monoSrc = s.monoRes.data();
    }
    if (outFrames == 0) return;
    if (outFrames > s.cvtMax) outFrames = s.cvtMax;  // process() contract guard

    if (s.stereoTmp.size() < (size_t)outFrames * 2) s.stereoTmp.resize((size_t)outFrames * 2);
    s.spatializer->process(monoSrc, s.stereoTmp.data(), outFrames);

    if (renderCh == 2) {
        s.ring.write(s.stereoTmp.data(), (size_t)outFrames * 2);
    } else {
        // Wider output: place the binaural pair in channels 0/1, silence the rest.
        const size_t need = (size_t)outFrames * renderCh;
        if (s.convertBuf.size() < need) s.convertBuf.resize(need);
        std::fill_n(s.convertBuf.data(), need, 0.0f);
        for (uint32_t f = 0; f < outFrames; ++f) {
            s.convertBuf[(size_t)f * renderCh + 0] = s.stereoTmp[(size_t)f * 2 + 0];
            s.convertBuf[(size_t)f * renderCh + 1] = s.stereoTmp[(size_t)f * 2 + 1];
        }
        s.ring.write(s.convertBuf.data(), need);
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

    if (mixBuf.size() < outSamples) mixBuf.resize(outSamples);
    std::fill_n(mixBuf.data(), outSamples, 0.0f);

    for (auto& s : strips) {
        float vol = s->muted.load() ? 0.0f : s->volume.load();
        if (s->readBuf.size() < outSamples) s->readBuf.resize(outSamples);
        s->ring.readOrSilence(s->readBuf.data(), outSamples);
        const float* src = s->readBuf.data();
        float* dst = mixBuf.data();

        float sumSq = 0.0f;
        float maxAbs = 0.0f;
        if (vol == 0.0f) {
            // Still consume so the strip doesn't drift/fill up, but no audio.
            for (size_t i = 0; i < outSamples; ++i) {
                dst[i] += src[i] * vol;
            }
            s->peak.store(0.0f);
            s->rms.store(0.0f);
        } else {
            for (size_t i = 0; i < outSamples; ++i) {
                float sample = src[i] * vol;
                dst[i] += sample;
                float a = std::fabs(sample);
                if (a > maxAbs) maxAbs = a;
                sumSq += sample * sample;
            }
            s->peak.store(maxAbs);
            s->rms.store(std::sqrt(sumSq / static_cast<float>(outSamples)));
        }
    }

    float master = masterVolume.load();
    if (master != 1.0f) {
        for (size_t i = 0; i < outSamples; ++i) mixBuf[i] *= master;
    }
    if (masterMuted.load()) {
        for (size_t i = 0; i < outSamples; ++i) mixBuf[i] = 0.0f;
    }

    {
        float sumSq = 0.0f;
        float maxAbs = 0.0f;
        for (size_t i = 0; i < outSamples; ++i) {
            float a = std::fabs(mixBuf[i]);
            if (a > maxAbs) maxAbs = a;
            sumSq += mixBuf[i] * mixBuf[i];
        }
        masterPeak.store(maxAbs);
        masterRms.store(std::sqrt(sumSq / static_cast<float>(outSamples)));
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

    std::lock_guard<std::mutex> lk(structureMutex);
    for (auto& s : strips) teardownStrip(*s);
    strips.clear();

    renderSvc.Reset();
    renderAC.Reset();
    renderDev.Reset();
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

std::optional<StripId> AudioMixer::addStrip(const MixerStripConfig& cfg)
{
    if (!m_impl->renderAC) return std::nullopt; // init() must run first

    {
        std::lock_guard<std::mutex> lk(m_impl->structureMutex);
        if (m_impl->strips.size() >= kMaxStrips) {
            std::fprintf(stderr, "[mixer] Too many strips (max %zu)\n", kMaxStrips);
            return std::nullopt;
        }
    }

    auto strip = std::make_shared<Impl::Strip>();
    strip->id        = m_impl->nextStripId.fetch_add(1);
    strip->name      = cfg.name.empty() ? cfg.source : cfg.name;
    strip->source    = cfg.source;
    strip->knobIndex = cfg.knobIndex;
    strip->volume.store(std::clamp(cfg.volume, 0.0f, 2.0f));
    strip->muted.store(cfg.muted);

    if (!m_impl->openStrip(*strip, cfg)) {
        // A partially opened strip may already own a capture event handle.
        m_impl->teardownStrip(*strip);
        return std::nullopt;
    }

    if (m_impl->running.load()) {
        m_impl->enqueue(Impl::Command{ Impl::Command::Kind::Add, strip, 0, {}, {} });
    } else {
        std::lock_guard<std::mutex> lk(m_impl->structureMutex);
        m_impl->strips.push_back(strip);
    }
    return strip->id;
}

bool AudioMixer::removeStrip(StripId id)
{
    if (m_impl->running.load()) {
        // Existence isn't verified synchronously in the running case — the
        // command is a no-op if the id doesn't exist by the time it's applied.
        m_impl->enqueue(Impl::Command{ Impl::Command::Kind::Remove, nullptr, id, {}, {} });
        return true;
    }

    std::lock_guard<std::mutex> lk(m_impl->structureMutex);
    auto it = std::find_if(m_impl->strips.begin(), m_impl->strips.end(),
        [&](const std::shared_ptr<Impl::Strip>& s) { return s->id == id; });
    if (it == m_impl->strips.end()) return false;
    m_impl->teardownStrip(**it);
    m_impl->strips.erase(it);
    return true;
}

bool AudioMixer::renameStrip(StripId id, const std::string& name)
{
    if (m_impl->running.load()) {
        m_impl->enqueue(Impl::Command{ Impl::Command::Kind::Rename, nullptr, id, name, {} });
        return true;
    }
    std::lock_guard<std::mutex> lk(m_impl->structureMutex);
    auto s = m_impl->findStripLocked(id);
    if (!s) return false;
    s->name = name;
    return true;
}

bool AudioMixer::setStripKnobIndex(StripId id, std::optional<int> knobIndex)
{
    if (m_impl->running.load()) {
        m_impl->enqueue(Impl::Command{ Impl::Command::Kind::SetKnobIndex, nullptr, id, {}, knobIndex });
        return true;
    }
    std::lock_guard<std::mutex> lk(m_impl->structureMutex);
    auto s = m_impl->findStripLocked(id);
    if (!s) return false;
    s->knobIndex = knobIndex;
    return true;
}

bool AudioMixer::start()
{
    if (!m_impl->renderAC) return false;
    {
        std::lock_guard<std::mutex> lk(m_impl->structureMutex);
        if (m_impl->strips.empty()) return false;
    }
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

bool AudioMixer::setStripVolume(StripId id, float vol)
{
    auto s = m_impl->findStrip(id);
    if (!s) return false;
    s->volume.store(std::clamp(vol, 0.0f, 2.0f));
    return true;
}

bool AudioMixer::setStripMuted(StripId id, bool muted)
{
    auto s = m_impl->findStrip(id);
    if (!s) return false;
    s->muted.store(muted);
    return true;
}

bool AudioMixer::setStripDirection(StripId id, float azimuth, float elevation)
{
    auto s = m_impl->findStrip(id);
    if (!s || !s->spatializer) return false;
    s->reqAz.store(azimuth);
    s->reqEl.store(elevation);
    s->dirDirty.store(true);   // picked up on the audio thread in processStrip
    return true;
}

void AudioMixer::setMasterVolume(float v)
{
    m_impl->masterVolume.store(std::clamp(v, 0.0f, 2.0f));
}

float AudioMixer::masterVolume() const { return m_impl->masterVolume.load(); }

void AudioMixer::setMasterMuted(bool muted) { m_impl->masterMuted.store(muted); }
bool AudioMixer::masterMuted() const { return m_impl->masterMuted.load(); }
float AudioMixer::masterPeak() const { return m_impl->masterPeak.load(); }
float AudioMixer::masterRms() const { return m_impl->masterRms.load(); }

size_t AudioMixer::stripCount() const noexcept
{
    std::lock_guard<std::mutex> lk(m_impl->structureMutex);
    return m_impl->strips.size();
}

std::vector<StripSnapshot> AudioMixer::snapshot() const
{
    std::vector<StripSnapshot> out;
    std::lock_guard<std::mutex> lk(m_impl->structureMutex);
    out.reserve(m_impl->strips.size());
    for (auto& s : m_impl->strips) out.push_back(m_impl->toSnapshot(*s));
    return out;
}

std::optional<StripSnapshot> AudioMixer::stripSnapshot(StripId id) const
{
    auto s = m_impl->findStrip(id);
    if (!s) return std::nullopt;
    return m_impl->toSnapshot(*s);
}

std::vector<EndpointInfo> AudioMixer::listEndpoints() const
{
    if (!m_impl->enumerator) return {};
    return enumEndpoints(m_impl->enumerator.Get());
}

const std::string& AudioMixer::outputName() const { return m_impl->outputName; }

} // namespace anniaudio::core
