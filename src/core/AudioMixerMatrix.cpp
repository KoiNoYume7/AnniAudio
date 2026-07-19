#include "AudioMixerMatrix.hpp"
#include "audio_utils.hpp"

#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <mmdeviceapi.h>

namespace anniaudio::core {

using Microsoft::WRL::ComPtr;

struct AudioMixerMatrix::Impl {
    struct RouteInfo {
        std::string output;
        AudioMixer* mixer = nullptr;
        StripId     localId = 0;
    };

    mutable std::mutex mtx;
    std::map<std::string, std::unique_ptr<AudioMixer>> outputs;
    std::unordered_map<RouteId, RouteInfo> routes;
    std::map<std::pair<AudioMixer*, StripId>, RouteId> localToGlobal;
    std::atomic<uint64_t> nextRouteId{1};

    ComPtr<IMMDeviceEnumerator> enumerator;
    std::atomic<bool> running{false};

    AudioMixer* findOutputMixer(const std::string& name) const {
        auto it = outputs.find(name);
        return it != outputs.end() ? it->second.get() : nullptr;
    }

    bool ensureEnumerator() {
        if (enumerator) return true;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        return SUCCEEDED(hr);
    }
};

AudioMixerMatrix::AudioMixerMatrix() : m_impl(std::make_unique<Impl>()) {}
AudioMixerMatrix::~AudioMixerMatrix() { stop(); }

bool AudioMixerMatrix::addOutput(const std::string& outputHint)
{
    auto mixer = std::make_unique<AudioMixer>();
    if (!mixer->init(outputHint)) return false;

    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->outputs[mixer->outputName()] = std::move(mixer);
    return true;
}

bool AudioMixerMatrix::removeOutput(const std::string& outputName)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->outputs.find(outputName);
    if (it == m_impl->outputs.end()) return false;

    // Stop the mixer and remove every route that belongs to it.
    it->second->stop();
    for (auto rit = m_impl->routes.begin(); rit != m_impl->routes.end(); ) {
        if (rit->second.mixer == it->second.get()) {
            m_impl->localToGlobal.erase({ it->second.get(), rit->second.localId });
            rit = m_impl->routes.erase(rit);
        } else {
            ++rit;
        }
    }
    m_impl->outputs.erase(it);
    return true;
}

std::vector<std::string> AudioMixerMatrix::outputs() const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    std::vector<std::string> names;
    names.reserve(m_impl->outputs.size());
    for (const auto& kv : m_impl->outputs) names.push_back(kv.first);
    return names;
}

bool AudioMixerMatrix::start()
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    bool any = false;
    for (auto& kv : m_impl->outputs) {
        if (kv.second->start()) any = true;
    }
    if (any) m_impl->running = true;
    return any;
}

void AudioMixerMatrix::stop()
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    for (auto& kv : m_impl->outputs) kv.second->stop();
    m_impl->running = false;
}

bool AudioMixerMatrix::running() const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    for (const auto& kv : m_impl->outputs) {
        if (kv.second->running()) return true;
    }
    return false;
}

std::optional<AudioMixerMatrix::RouteId> AudioMixerMatrix::addRoute(
    const std::string& source,
    const std::string& output,
    const MixerStripConfig& cfg)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    AudioMixer* mixer = m_impl->findOutputMixer(output);
    if (!mixer) return std::nullopt;

    MixerStripConfig c = cfg;
    c.source = source;
    auto local = mixer->addStrip(c);
    if (!local) return std::nullopt;

    RouteId id = m_impl->nextRouteId.fetch_add(1);
    m_impl->routes[id] = { output, mixer, *local };
    m_impl->localToGlobal[{ mixer, *local }] = id;

    // If the output isn't already running, try to start it now that it has a strip.
    if (!mixer->running()) mixer->start();

    return id;
}

bool AudioMixerMatrix::removeRoute(RouteId id)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->routes.find(id);
    if (it == m_impl->routes.end()) return false;

    AudioMixer* mixer = it->second.mixer;
    StripId local = it->second.localId;
    m_impl->localToGlobal.erase({ mixer, local });
    m_impl->routes.erase(it);

    if (mixer) {
        mixer->removeStrip(local);
        if (mixer->stripCount() == 0) mixer->stop();
    }
    return true;
}

bool AudioMixerMatrix::renameRoute(RouteId id, const std::string& name)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->routes.find(id);
    if (it == m_impl->routes.end()) return false;
    return it->second.mixer->renameStrip(it->second.localId, name);
}

bool AudioMixerMatrix::setRouteVolume(RouteId id, float vol)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->routes.find(id);
    if (it == m_impl->routes.end()) return false;
    return it->second.mixer->setStripVolume(it->second.localId, vol);
}

bool AudioMixerMatrix::setRouteMuted(RouteId id, bool muted)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->routes.find(id);
    if (it == m_impl->routes.end()) return false;
    return it->second.mixer->setStripMuted(it->second.localId, muted);
}

bool AudioMixerMatrix::setRouteKnobIndex(RouteId id, std::optional<int> knobIndex)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->routes.find(id);
    if (it == m_impl->routes.end()) return false;
    return it->second.mixer->setStripKnobIndex(it->second.localId, knobIndex);
}

float AudioMixerMatrix::outputMasterVolume(const std::string& output) const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto* mixer = m_impl->findOutputMixer(output);
    return mixer ? mixer->masterVolume() : 1.0f;
}

float AudioMixerMatrix::outputMasterPeak(const std::string& output) const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto* mixer = m_impl->findOutputMixer(output);
    return mixer ? mixer->masterPeak() : 0.0f;
}

float AudioMixerMatrix::outputMasterRms(const std::string& output) const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto* mixer = m_impl->findOutputMixer(output);
    return mixer ? mixer->masterRms() : 0.0f;
}

void AudioMixerMatrix::setOutputMasterVolume(const std::string& output, float v)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto* mixer = m_impl->findOutputMixer(output);
    if (mixer) mixer->setMasterVolume(v);
}

size_t AudioMixerMatrix::routeCount() const noexcept
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    return m_impl->routes.size();
}

std::vector<StripSnapshot> AudioMixerMatrix::snapshot() const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    std::vector<StripSnapshot> out;
    out.reserve(m_impl->routes.size());

    for (const auto& kv : m_impl->outputs) {
        AudioMixer* mixer = kv.second.get();
        for (auto& snap : mixer->snapshot()) {
            auto lit = m_impl->localToGlobal.find({ mixer, snap.id });
            if (lit != m_impl->localToGlobal.end()) snap.id = lit->second;
            out.push_back(std::move(snap));
        }
    }
    return out;
}

std::optional<StripSnapshot> AudioMixerMatrix::routeSnapshot(RouteId id) const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->routes.find(id);
    if (it == m_impl->routes.end()) return std::nullopt;

    auto snap = it->second.mixer->stripSnapshot(it->second.localId);
    if (!snap) return std::nullopt;
    snap->id = id;
    return snap;
}

std::vector<EndpointInfo> AudioMixerMatrix::listEndpoints() const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    if (!m_impl->ensureEnumerator()) return {};
    return enumEndpoints(m_impl->enumerator.Get());
}

} // namespace anniaudio::core
