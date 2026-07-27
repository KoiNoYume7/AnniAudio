#include "OutputMixer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <windows.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "avrt.lib")

namespace anniaudio::core {

namespace {

constexpr UINT64 kDefaultBuffer100Ns = 2000000; // 200 ms shared buffer

} // namespace

class OutputMixer::Impl {
public:
    Impl() = default;
    ~Impl() { stop(); }

    bool init(const std::string& outputHint);
    bool start();
    void stop();
    bool running() const { return running_.load(); }

    void addGroupBus(std::shared_ptr<GroupBus> bus) {
        std::lock_guard<std::mutex> lk(busesMutex_);
        if (std::find(buses_.begin(), buses_.end(), bus) == buses_.end())
            buses_.push_back(bus);
    }
    void removeGroupBus(const GroupBus* bus) {
        std::lock_guard<std::mutex> lk(busesMutex_);
        buses_.erase(std::remove_if(buses_.begin(), buses_.end(),
            [bus](const std::shared_ptr<GroupBus>& b) { return b.get() == bus; }),
            buses_.end());
    }
    void clearGroupBuses() {
        std::lock_guard<std::mutex> lk(busesMutex_);
        buses_.clear();
    }

    void setMasterVolume(float v) { masterVolume_.store(v); }
    void setMasterMuted(bool muted) { masterMuted_.store(muted); }
    float masterVolume() const { return masterVolume_.load(); }
    bool masterMuted() const { return masterMuted_.load(); }
    float masterPeak() const { return masterPeak_.load(); }
    float masterRms() const { return masterRms_.load(); }

    const std::string& outputName() const { return outputName_; }

private:
    void run();
    void processRender();

    std::string outputName_;

    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    HANDLE renderEvent_ = nullptr;

    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice> renderDev_;
    ComPtr<IAudioClient> renderAC_;
    ComPtr<IAudioRenderClient> renderSvc_;
    WfxPtr renderFmt_;

    uint32_t renderCh_ = 0;
    uint32_t renderRate_ = 0;
    bool renderIsFloat_ = false;
    uint32_t renderBufFrames_ = 0;

    std::vector<float> mixBuf_;
    std::vector<float> groupBuf_;

    std::mutex busesMutex_;
    std::vector<std::shared_ptr<GroupBus>> buses_;

    std::atomic<bool> running_{false};
    std::atomic<bool> started_{false};
    std::atomic<float> masterVolume_{1.0f};
    std::atomic<bool> masterMuted_{false};
    std::atomic<float> masterPeak_{0.0f};
    std::atomic<float> masterRms_{0.0f};
};

OutputMixer::OutputMixer() : p(std::make_unique<Impl>()) {}
OutputMixer::~OutputMixer() = default;

bool OutputMixer::init(const std::string& outputHint) { return p->init(outputHint); }
bool OutputMixer::start() { return p->start(); }
void OutputMixer::stop() { p->stop(); }
bool OutputMixer::running() const { return p->running(); }
void OutputMixer::addGroupBus(std::shared_ptr<GroupBus> bus) { p->addGroupBus(bus); }
void OutputMixer::removeGroupBus(const GroupBus* bus) { p->removeGroupBus(bus); }
void OutputMixer::clearGroupBuses() { p->clearGroupBuses(); }
void OutputMixer::setMasterVolume(float v) { p->setMasterVolume(v); }
void OutputMixer::setMasterMuted(bool muted) { p->setMasterMuted(muted); }
float OutputMixer::masterVolume() const { return p->masterVolume(); }
bool OutputMixer::masterMuted() const { return p->masterMuted(); }
float OutputMixer::masterPeak() const { return p->masterPeak(); }
float OutputMixer::masterRms() const { return p->masterRms(); }
const std::string& OutputMixer::outputName() const { return p->outputName(); }

bool OutputMixer::Impl::init(const std::string& outputHint)
{
    stop();

    EnsureComInitializedOnThisThread();

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator_));
    if (FAILED(hr) || !enumerator_) {
        std::fprintf(stderr, "[output] CoCreateInstance(MMDeviceEnumerator) failed 0x%08X\n", (unsigned)hr);
        return false;
    }

    renderDev_ = findDevice(enumerator_.Get(), eRender, outputHint);
    if (!renderDev_) {
        std::fprintf(stderr, "[output] Could not find output '%s'\n", outputHint.c_str());
        return false;
    }

    hr = renderDev_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(renderAC_.GetAddressOf()));
    if (FAILED(hr) || !renderAC_) return false;

    // Render at a fixed 48 kHz stereo float format and let the WASAPI engine
    // resample/channel-convert to the endpoint. This keeps the OutputMixer path
    // identical regardless of the physical device format.
    WAVEFORMATEXTENSIBLE wfex;
    initProcessLoopbackFormat(wfex);
    WAVEFORMATEX* raw = reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    if (!raw) return false;
    std::memcpy(raw, &wfex, sizeof(WAVEFORMATEXTENSIBLE));
    renderFmt_.reset(raw);

    renderCh_      = wfex.Format.nChannels;
    renderRate_    = wfex.Format.nSamplesPerSec;
    renderIsFloat_ = true;

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
    hr = renderAC_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                               kDefaultBuffer100Ns, 0, renderFmt_.get(), nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[output] renderAC->Initialize for '%s' failed 0x%08X\n",
                     outputHint.c_str(), (unsigned)hr);
        renderAC_.Reset();
        return false;
    }

    hr = renderAC_->GetBufferSize(&renderBufFrames_);
    if (FAILED(hr) || renderBufFrames_ == 0) renderBufFrames_ = renderRate_ / 100;

    renderEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!renderEvent_) {
        renderAC_.Reset();
        return false;
    }
    hr = renderAC_->SetEventHandle(renderEvent_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[output] render SetEventHandle failed 0x%08X\n", (unsigned)hr);
        CloseHandle(renderEvent_); renderEvent_ = nullptr;
        renderAC_.Reset();
        return false;
    }

    hr = renderAC_->GetService(IID_PPV_ARGS(&renderSvc_));
    if (FAILED(hr) || !renderSvc_) {
        CloseHandle(renderEvent_); renderEvent_ = nullptr;
        renderAC_.Reset();
        return false;
    }

    outputName_ = friendlyName(renderDev_.Get());
    if (outputName_.empty()) outputName_ = outputHint;

    mixBuf_.resize(static_cast<size_t>(renderBufFrames_) * renderCh_);
    groupBuf_.resize(static_cast<size_t>(renderBufFrames_) * renderCh_);

    std::fprintf(stderr, "[output] Output  : %s\n", outputName_.c_str());
    std::fprintf(stderr, "[output] Output  format : %u Hz, %u ch, %s\n",
                 renderRate_, renderCh_, renderIsFloat_ ? "float" : "pcm");
    return true;
}

bool OutputMixer::Impl::start()
{
    if (running_.load() || !renderAC_) return false;

    stopEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!stopEvent_) return false;

    HRESULT hr = renderAC_->Start();
    if (FAILED(hr)) {
        std::fprintf(stderr, "[output] renderAC->Start failed 0x%08X\n", (unsigned)hr);
        CloseHandle(stopEvent_); stopEvent_ = nullptr;
        return false;
    }

    started_.store(true);
    running_.store(true);
    thread_ = std::thread([this]() { run(); });
    return true;
}

void OutputMixer::Impl::stop()
{
    if (!running_.exchange(false)) return;

    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();

    if (started_.load() && renderAC_) {
        renderAC_->Stop();
        started_.store(false);
    }

    if (renderEvent_) { CloseHandle(renderEvent_); renderEvent_ = nullptr; }
    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
    renderSvc_.Reset();
    renderAC_.Reset();
    renderDev_.Reset();
    enumerator_.Reset();
    thread_ = std::thread();
}

void OutputMixer::Impl::run()
{
    DWORD taskIndex = 0;
    HANDLE avTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    HANDLE handles[2] = { stopEvent_, renderEvent_ };

    while (running_.load()) {
        DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_OBJECT_0 + 1) continue;
        processRender();
    }

    if (avTask) AvRevertMmThreadCharacteristics(avTask);
}

void OutputMixer::Impl::processRender()
{
    UINT32 padding = 0;
    renderAC_->GetCurrentPadding(&padding);
    UINT32 toWrite = renderBufFrames_ - padding;
    if (toWrite == 0) return;

    BYTE* buf = nullptr;
    if (FAILED(renderSvc_->GetBuffer(toWrite, &buf))) return;

    const size_t outSamples = static_cast<size_t>(toWrite) * renderCh_;
    std::fill_n(mixBuf_.data(), outSamples, 0.0f);

    // Snapshot the bus list. shared_ptr copies keep any bus alive that is removed
    // while we are mixing it.
    std::vector<std::shared_ptr<GroupBus>> localBuses;
    {
        std::lock_guard<std::mutex> lk(busesMutex_);
        localBuses = buses_;
    }

    for (auto& bus : localBuses) {
        if (!bus) continue;
        std::fill_n(groupBuf_.data(), outSamples, 0.0f);
        bus->mix(groupBuf_.data(), toWrite, renderRate_, renderCh_);
        for (size_t i = 0; i < outSamples; ++i) mixBuf_[i] += groupBuf_[i];
    }

    bool muted = masterMuted_.load();
    float vol = masterVolume_.load();

    float maxAbs = 0.0f;
    float sumSq = 0.0f;

    if (renderIsFloat_) {
        auto* fbuf = reinterpret_cast<float*>(buf);
        for (size_t i = 0; i < outSamples; ++i) {
            float v = muted ? 0.0f : mixBuf_[i] * vol;
            if (v >  1.0f) v =  1.0f;
            if (v < -1.0f) v = -1.0f;
            fbuf[i] = v;
            float a = std::fabs(v);
            if (a > maxAbs) maxAbs = a;
            sumSq += v * v;
        }
    } else {
        std::vector<float> tmp(mixBuf_.data(), mixBuf_.data() + outSamples);
        if (!muted) {
            for (auto& s : tmp) s *= vol;
        } else {
            std::fill(tmp.begin(), tmp.end(), 0.0f);
        }
        for (auto s : tmp) {
            float a = std::fabs(s);
            if (a > maxAbs) maxAbs = a;
            sumSq += s * s;
        }
        floatToPcm16(tmp.data(), outSamples, buf);
    }

    masterPeak_.store(maxAbs);
    masterRms_.store(outSamples > 0 ? std::sqrt(sumSq / static_cast<float>(outSamples)) : 0.0f);

    renderSvc_->ReleaseBuffer(toWrite, 0);
}

} // namespace anniaudio::core
