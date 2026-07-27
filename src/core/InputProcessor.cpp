#include "InputProcessor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "eq.hpp"
#include "noise_suppressor.hpp"
#include "spatializer.hpp"

#include <windows.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include "eq.hpp"
#include "noise_suppressor.hpp"
#include "spatializer.hpp"

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "avrt.lib")

namespace anniaudio::core {

namespace {

constexpr UINT64 kDefaultBuffer100Ns = 2000000; // 200 ms shared buffer
constexpr float kSpatialSpreadDeg = 30.0f;

struct CompBand { anniaudio::dsp::FilterType type; double freq, gainDb, q; };
constexpr CompBand kColorComp[] = {
    { anniaudio::dsp::FilterType::LowShelf,    70.0,   7.0, 0.707 },
    { anniaudio::dsp::FilterType::Peak,       160.0,  -3.0, 1.2   },
    { anniaudio::dsp::FilterType::Peak,      2500.0, -12.0, 0.8   },
    { anniaudio::dsp::FilterType::Peak,      4000.0, -13.0, 1.1   },
    { anniaudio::dsp::FilterType::HighShelf, 8000.0,  -4.0, 0.707 },
};

bool parseApplicationPid(const std::string& s, uint32_t& pid)
{
    try {
        size_t pos = 0;
        unsigned long long v = std::stoull(s, &pos);
        if (pos != s.size() || v > UINT32_MAX) return false;
        pid = static_cast<uint32_t>(v);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

class InputProcessor::Impl {
public:
    Impl() = default;
    ~Impl() { stop(); }

    bool init(const InputProcessorConfig& cfg);
    bool start();
    void stop();
    bool running() const { return running_.load(); }

    MultiReaderRingBuffer& outputRing() { return ring_; }

    void setDirection(float az, float el) {
        reqAz_.store(az);
        reqEl_.store(el);
        dirDirty_.store(true);
    }

    float peak() const { return peak_.load(); }
    float rms() const { return rms_.load(); }

private:
    struct DspState {
        std::unique_ptr<anniaudio::dsp::NoiseSuppressor> denoiser;
        std::unique_ptr<anniaudio::dsp::EqChain> eq;
        std::unique_ptr<anniaudio::dsp::Spatializer> spatialL;
        std::unique_ptr<anniaudio::dsp::Spatializer> spatialR;
        std::unique_ptr<anniaudio::dsp::EqChain> colorEq;
    };

    bool openEndpointCapture(const std::string& sourceHint);
    bool openApplicationLoopback(uint32_t pid);
    bool setupDsp(const InputProcessorConfig& cfg);
    void setupDspBuffers(uint32_t captureBufFrames);
    void cleanup();
    void run();
    void processPacket();
    void applyDirectionIfNeeded();
    void updateLevels(const float* data, size_t n);

    InputProcessorConfig cfg_;

    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    HANDLE captureEvent_ = nullptr;

    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IAudioClient> captureAC_;
    ComPtr<IAudioCaptureClient> captureSvc_;
    WfxPtr captureFmt_;

    uint32_t captureCh_ = 0;
    uint32_t captureRate_ = 0;
    bool captureIsFloat_ = false;
    bool captureIsLoopback_ = false;
    uint32_t captureBufFrames_ = 0;

    double srcToProcRatio_ = 1.0;
    double srcPhaseL_ = 0.0, srcPhaseR_ = 0.0;
    uint32_t cvtMax_ = 0;

    DspState dsp_;

    // Scratch buffers (pre-allocated before the audio thread runs).
    std::vector<float> captureTmp_;
    std::vector<float> chL_;
    std::vector<float> chR_;
    std::vector<float> resL_;
    std::vector<float> resR_;
    std::vector<float> spkTmp_;
    std::vector<float> mixTmp_;

    MultiReaderRingBuffer ring_;

    std::atomic<bool> running_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> dirDirty_{false};
    std::atomic<float> reqAz_{0.0f};
    std::atomic<float> reqEl_{0.0f};
    float appliedAz_ = 0.0f;
    float appliedEl_ = 0.0f;

    std::atomic<float> peak_{0.0f};
    std::atomic<float> rms_{0.0f};
};

InputProcessor::InputProcessor() : p(std::make_unique<Impl>()) {}
InputProcessor::~InputProcessor() = default;

bool InputProcessor::init(const InputProcessorConfig& cfg) { return p->init(cfg); }
bool InputProcessor::start() { return p->start(); }
void InputProcessor::stop() { p->stop(); }
bool InputProcessor::running() const { return p->running(); }
MultiReaderRingBuffer& InputProcessor::outputRing() { return p->outputRing(); }
void InputProcessor::setDirection(float az, float el) { p->setDirection(az, el); }
float InputProcessor::peak() const { return p->peak(); }
float InputProcessor::rms() const { return p->rms(); }

bool InputProcessor::Impl::init(const InputProcessorConfig& cfg)
{
    stop();
    cfg_ = cfg;

    EnsureComInitializedOnThisThread();

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator_));
    if (FAILED(hr) || !enumerator_) {
        std::fprintf(stderr, "[input] CoCreateInstance(MMDeviceEnumerator) failed 0x%08X\n", (unsigned)hr);
        return false;
    }

    bool ok = false;
    if (cfg_.type == "application") {
        uint32_t pid = 0;
        if (!parseApplicationPid(cfg_.source, pid)) {
            std::fprintf(stderr, "[input] Invalid application process id '%s'\n", cfg_.source.c_str());
            return false;
        }
        ok = openApplicationLoopback(pid);
    } else {
        ok = openEndpointCapture(cfg_.source);
    }
    if (!ok) return false;

    return setupDsp(cfg_);
}

bool InputProcessor::Impl::openEndpointCapture(const std::string& sourceHint)
{
    EDataFlow foundFlow = eAll;
    ComPtr<IMMDevice> dev = findAnyDevice(enumerator_.Get(), sourceHint, &foundFlow);
    if (!dev) {
        std::fprintf(stderr, "[input] Could not find source '%s'\n", sourceHint.c_str());
        return false;
    }
    captureIsLoopback_ = (foundFlow == eRender);

    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(captureAC_.GetAddressOf()));
    if (FAILED(hr) || !captureAC_) return false;

    WAVEFORMATEX* raw = nullptr;
    hr = captureAC_->GetMixFormat(&raw);
    if (FAILED(hr) || !raw) return false;
    captureFmt_.reset(raw);

    WfxPtr forced = tryForceFloat(captureAC_.Get(), captureFmt_.get());
    if (forced) captureFmt_ = std::move(forced);

    captureCh_      = captureFmt_->nChannels;
    captureRate_    = captureFmt_->nSamplesPerSec;
    captureIsFloat_ = isFloatWfx(captureFmt_.get());

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (captureIsLoopback_) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    hr = captureAC_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                kDefaultBuffer100Ns, 0, captureFmt_.get(), nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[input] captureAC->Initialize for '%s' failed 0x%08X\n",
                     cfg_.name.c_str(), (unsigned)hr);
        captureAC_.Reset();
        return false;
    }

    captureEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent_) {
        captureAC_.Reset();
        return false;
    }
    hr = captureAC_->SetEventHandle(captureEvent_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[input] capture SetEventHandle failed 0x%08X\n", (unsigned)hr);
        CloseHandle(captureEvent_); captureEvent_ = nullptr;
        captureAC_.Reset();
        return false;
    }

    hr = captureAC_->GetService(IID_PPV_ARGS(&captureSvc_));
    if (FAILED(hr) || !captureSvc_) {
        CloseHandle(captureEvent_); captureEvent_ = nullptr;
        captureAC_.Reset();
        return false;
    }

    hr = captureAC_->GetBufferSize(&captureBufFrames_);
    if (FAILED(hr) || captureBufFrames_ == 0) captureBufFrames_ = captureRate_ / 100;

    std::fprintf(stderr, "[input] Capture  : %s (%s)\n", cfg_.name.c_str(), sourceHint.c_str());
    std::fprintf(stderr, "[input] Capture  format : %u Hz, %u ch, %s\n",
                 captureRate_, captureCh_, captureIsFloat_ ? "float" : "pcm");
    return true;
}

bool InputProcessor::Impl::openApplicationLoopback(uint32_t pid)
{
    captureAC_ = activateProcessLoopbackClient(pid);
    if (!captureAC_) return false;

    WAVEFORMATEXTENSIBLE wfex;
    initProcessLoopbackFormat(wfex);

    DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
    HRESULT hr = captureAC_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0,
                                        reinterpret_cast<const WAVEFORMATEX*>(&wfex), nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[input] Process loopback IAudioClient::Initialize failed for pid %u: 0x%08X\n",
                     pid, (unsigned)hr);
        captureAC_.Reset();
        return false;
    }

    captureEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent_) {
        captureAC_.Reset();
        return false;
    }
    hr = captureAC_->SetEventHandle(captureEvent_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[input] SetEventHandle failed for pid %u: 0x%08X\n", pid, (unsigned)hr);
        CloseHandle(captureEvent_); captureEvent_ = nullptr;
        captureAC_.Reset();
        return false;
    }

    hr = captureAC_->GetService(IID_PPV_ARGS(&captureSvc_));
    if (FAILED(hr) || !captureSvc_) {
        CloseHandle(captureEvent_); captureEvent_ = nullptr;
        captureAC_.Reset();
        return false;
    }

    hr = captureAC_->GetBufferSize(&captureBufFrames_);
    if (FAILED(hr) || captureBufFrames_ == 0) captureBufFrames_ = wfex.Format.nSamplesPerSec / 100;

    captureCh_      = wfex.Format.nChannels;
    captureRate_    = wfex.Format.nSamplesPerSec;
    captureIsFloat_ = true;
    captureIsLoopback_ = true;

    std::fprintf(stderr, "[input] Capture  : %s (pid %u)\n", cfg_.name.c_str(), pid);
    std::fprintf(stderr, "[input] Capture  format : %u Hz, %u ch, float\n", captureRate_, captureCh_);
    return true;
}

bool InputProcessor::Impl::setupDsp(const InputProcessorConfig& cfg)
{
    srcToProcRatio_ = static_cast<double>(kProcessingRate) / static_cast<double>(captureRate_);
    cvtMax_ = static_cast<uint32_t>(std::ceil(static_cast<double>(captureBufFrames_) * srcToProcRatio_)) + kProcessingChannels;

    setupDspBuffers(captureBufFrames_);

    if (cfg.denoise) {
        if (captureRate_ == 48000) {
            dsp_.denoiser = std::make_unique<anniaudio::dsp::NoiseSuppressor>();
            dsp_.denoiser->prepare(captureCh_);
            if (!dsp_.denoiser->prepared()) {
                std::fprintf(stderr, "[input] denoise init failed for '%s'\n", cfg_.name.c_str());
                dsp_.denoiser.reset();
            } else {
                std::fprintf(stderr, "[input] denoise enabled for '%s' (%u ch)\n", cfg_.name.c_str(), captureCh_);
            }
        } else {
            std::fprintf(stderr, "[input] denoise skipped for '%s': capture rate %u != 48000\n",
                         cfg_.name.c_str(), captureRate_);
        }
    }

    if (cfg.eqPreset == "voice") {
        dsp_.eq = std::make_unique<anniaudio::dsp::EqChain>();
        dsp_.eq->addBand(anniaudio::dsp::FilterType::HighPass, 80.0, 0.0, 0.707);
        dsp_.eq->addBand(anniaudio::dsp::FilterType::Peak, 250.0, -2.0, 1.0);
        dsp_.eq->addBand(anniaudio::dsp::FilterType::Peak, 3000.0, 2.5, 1.0);
        dsp_.eq->addBand(anniaudio::dsp::FilterType::HighShelf, 8000.0, 1.5, 0.707);
        dsp_.eq->prepare(static_cast<double>(captureRate_), captureCh_);
        std::fprintf(stderr, "[input] voice EQ enabled for '%s'\n", cfg_.name.c_str());
    } else if (!cfg.eqPreset.empty()) {
        std::fprintf(stderr, "[input] unknown eq preset '%s' for '%s' (available: voice)\n",
                     cfg.eqPreset.c_str(), cfg_.name.c_str());
    }

    if (cfg.spatial) {
        const std::string sofa = resolveHrtfPath();
        dsp_.spatialL = std::make_unique<anniaudio::dsp::Spatializer>();
        dsp_.spatialR = std::make_unique<anniaudio::dsp::Spatializer>();
        bool okL = dsp_.spatialL->loadHrtf(sofa, static_cast<double>(kProcessingRate), cvtMax_);
        bool okR = dsp_.spatialR->loadHrtf(sofa, static_cast<double>(kProcessingRate), cvtMax_);
        if (okL && okR) {
            dsp_.spatialL->setDirection(cfg.azimuth + kSpatialSpreadDeg, cfg.elevation);
            dsp_.spatialR->setDirection(cfg.azimuth - kSpatialSpreadDeg, cfg.elevation);
            appliedAz_ = cfg.azimuth;
            appliedEl_ = cfg.elevation;
            reqAz_.store(cfg.azimuth);
            reqEl_.store(cfg.elevation);

            dsp_.colorEq = std::make_unique<anniaudio::dsp::EqChain>();
            for (const auto& b : kColorComp) dsp_.colorEq->addBand(b.type, b.freq, b.gainDb, b.q);
            dsp_.colorEq->prepare(static_cast<double>(kProcessingRate), kProcessingChannels);

            std::fprintf(stderr, "[input] spatial enabled for '%s' (az=%.0f el=%.0f, %u taps @ %u Hz)\n",
                         cfg_.name.c_str(), cfg.azimuth, cfg.elevation,
                         dsp_.spatialL->irLength(), kProcessingRate);
        } else {
            std::fprintf(stderr, "[input] spatial init failed for '%s' (HRTF load)\n", cfg_.name.c_str());
            dsp_.spatialL.reset();
            dsp_.spatialR.reset();
        }
    }

    // Output ring: a few seconds of headroom at 48 kHz stereo.
    ring_.init(static_cast<size_t>(kProcessingRate) * kProcessingChannels * 4);
    return true;
}

void InputProcessor::Impl::setupDspBuffers(uint32_t captureBufFrames)
{
    captureTmp_.resize(static_cast<size_t>(captureBufFrames) * captureCh_);
    chL_.resize(static_cast<size_t>(captureBufFrames) + 1);
    chR_.resize(static_cast<size_t>(captureBufFrames) + 1);
    resL_.resize(static_cast<size_t>(cvtMax_) + 1);
    resR_.resize(static_cast<size_t>(cvtMax_) + 1);
    spkTmp_.resize(static_cast<size_t>(cvtMax_) * 2 + 2);
    mixTmp_.resize(static_cast<size_t>(cvtMax_) * 2 + 2);
}

bool InputProcessor::Impl::start()
{
    if (running_.load() || !captureAC_) return false;

    stopEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!stopEvent_) return false;

    HRESULT hr = captureAC_->Start();
    if (FAILED(hr)) {
        std::fprintf(stderr, "[input] captureAC->Start failed 0x%08X\n", (unsigned)hr);
        CloseHandle(stopEvent_); stopEvent_ = nullptr;
        return false;
    }

    started_.store(true);
    running_.store(true);
    thread_ = std::thread([this]() { run(); });
    return true;
}

void InputProcessor::Impl::stop()
{
    if (!running_.exchange(false)) return;

    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();

    if (started_ && captureAC_) {
        captureAC_->Stop();
        started_.store(false);
    }

    cleanup();
}

void InputProcessor::Impl::cleanup()
{
    if (captureEvent_) { CloseHandle(captureEvent_); captureEvent_ = nullptr; }
    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
    captureSvc_.Reset();
    captureAC_.Reset();
    captureFmt_.reset();
    enumerator_.Reset();

    dsp_.denoiser.reset();
    dsp_.eq.reset();
    dsp_.spatialL.reset();
    dsp_.spatialR.reset();
    dsp_.colorEq.reset();

    thread_ = std::thread();
}

void InputProcessor::Impl::run()
{
    DWORD taskIndex = 0;
    HANDLE avTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    HANDLE handles[2] = { stopEvent_, captureEvent_ };

    while (running_.load()) {
        DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_OBJECT_0 + 1) continue;

        applyDirectionIfNeeded();

        UINT32 frames = 0;
        while (running_.load() && SUCCEEDED(captureSvc_->GetNextPacketSize(&frames)) && frames > 0) {
            processPacket();
            applyDirectionIfNeeded();
        }
    }

    if (avTask) AvRevertMmThreadCharacteristics(avTask);
}

void InputProcessor::Impl::applyDirectionIfNeeded()
{
    if (!dsp_.spatialL || !dirDirty_.exchange(false)) return;
    float az = reqAz_.load();
    float el = reqEl_.load();
    if (az != appliedAz_ || el != appliedEl_) {
        dsp_.spatialL->setDirection(az + kSpatialSpreadDeg, el);
        dsp_.spatialR->setDirection(az - kSpatialSpreadDeg, el);
        appliedAz_ = az;
        appliedEl_ = el;
    }
}

void InputProcessor::Impl::processPacket()
{
    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    UINT64 dummy1 = 0, dummy2 = 0;
    HRESULT hr = captureSvc_->GetBuffer(&data, &frames, &flags, &dummy1, &dummy2);
    if (FAILED(hr) || frames == 0) return;

    const size_t sampleCount = static_cast<size_t>(frames) * captureCh_;
    if (captureTmp_.size() < sampleCount) captureTmp_.resize(sampleCount);

    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        std::fill_n(captureTmp_.data(), sampleCount, 0.0f);
    } else if (captureIsFloat_) {
        std::copy(reinterpret_cast<const float*>(data),
                  reinterpret_cast<const float*>(data) + sampleCount,
                  captureTmp_.data());
    } else {
        pcm16ToFloat(data, sampleCount, captureTmp_.data());
    }

    if (dsp_.denoiser) dsp_.denoiser->process(captureTmp_.data(), frames, captureCh_);
    if (dsp_.eq)       dsp_.eq->process(captureTmp_.data(), frames, captureCh_);

    // Extract front left/right (mono -> duplicate) at capture rate.
    if (chL_.size() < frames) { chL_.resize(frames); chR_.resize(frames); }
    const uint32_t cc = captureCh_;
    for (uint32_t f = 0; f < frames; ++f) {
        const float* fr = &captureTmp_[static_cast<size_t>(f) * cc];
        chL_[f] = fr[0];
        chR_[f] = (cc >= 2) ? fr[1] : fr[0];
    }

    if (dsp_.spatialL) {
        uint32_t oL = convertBuffer(chL_.data(), frames, 1,
                                    resL_.data(), static_cast<uint32_t>(resL_.size()), 1,
                                    srcToProcRatio_, srcPhaseL_);
        uint32_t oR = convertBuffer(chR_.data(), frames, 1,
                                    resR_.data(), static_cast<uint32_t>(resR_.size()), 1,
                                    srcToProcRatio_, srcPhaseR_);
        const uint32_t outFrames = std::min(oL, oR);

        if (spkTmp_.size() < static_cast<size_t>(outFrames) * 2) spkTmp_.resize(static_cast<size_t>(outFrames) * 2 + 2);
        if (mixTmp_.size() < static_cast<size_t>(outFrames) * 2) mixTmp_.resize(static_cast<size_t>(outFrames) * 2 + 2);

        std::fill_n(mixTmp_.data(), static_cast<size_t>(outFrames) * 2, 0.0f);
        dsp_.spatialL->process(resL_.data(), spkTmp_.data(), outFrames);
        for (uint32_t i = 0; i < static_cast<size_t>(outFrames) * 2; ++i) mixTmp_[i] += spkTmp_[i];
        dsp_.spatialR->process(resR_.data(), spkTmp_.data(), outFrames);
        for (uint32_t i = 0; i < static_cast<size_t>(outFrames) * 2; ++i) mixTmp_[i] += spkTmp_[i];

        if (dsp_.colorEq) dsp_.colorEq->process(mixTmp_.data(), outFrames, kProcessingChannels);

        updateLevels(mixTmp_.data(), static_cast<size_t>(outFrames) * kProcessingChannels);
        ring_.write(mixTmp_.data(), static_cast<size_t>(outFrames) * kProcessingChannels);
    } else {
        // Non-spatial: resample the stereo L/R pair directly into mixTmp.
        uint32_t outFrames = convertBuffer(
            captureTmp_.data(), frames, captureCh_,
            mixTmp_.data(), static_cast<uint32_t>(mixTmp_.size() / kProcessingChannels),
            kProcessingChannels,
            srcToProcRatio_, srcPhaseL_);

        // convertBuffer zeros extra channels, but for mono it only copies L and zeroes R.
        // Duplicate mono to both channels for a proper stereo feed.
        if (captureCh_ == 1) {
            for (uint32_t f = 0; f < outFrames; ++f) {
                float v = mixTmp_[static_cast<size_t>(f) * kProcessingChannels];
                mixTmp_[static_cast<size_t>(f) * kProcessingChannels + 1] = v;
            }
        }

        updateLevels(mixTmp_.data(), static_cast<size_t>(outFrames) * kProcessingChannels);
        ring_.write(mixTmp_.data(), static_cast<size_t>(outFrames) * kProcessingChannels);
    }

    captureSvc_->ReleaseBuffer(frames);
}

void InputProcessor::Impl::updateLevels(const float* data, size_t n)
{
    float maxAbs = 0.0f;
    float sumSq = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        float a = std::fabs(data[i]);
        if (a > maxAbs) maxAbs = a;
        sumSq += data[i] * data[i];
    }
    peak_.store(maxAbs);
    rms_.store(n > 0 ? std::sqrt(sumSq / static_cast<float>(n)) : 0.0f);
}

} // namespace anniaudio::core
