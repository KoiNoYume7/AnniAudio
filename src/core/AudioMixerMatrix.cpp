#include "AudioMixerMatrix.hpp"

#include "InputProcessor.hpp"
#include "GroupBus.hpp"
#include "OutputMixer.hpp"
#include "audio_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <windows.h>
#include <mmdeviceapi.h>

namespace anniaudio::core {

using Microsoft::WRL::ComPtr;

namespace {

float pctToLin(float pct) { return pct / 100.0f; }
float linToPct(float lin) { return lin * 100.0f; }

nlohmann::json toJson(const MixerStateSnapshot& s) {
    nlohmann::json j;
    j["running"] = s.running;
    j["controlPort"] = s.controlPort;
    j["inputs"] = nlohmann::json::array();
    for (const auto& in : s.inputs) {
        nlohmann::json ij;
        ij["id"] = in.id;
        ij["name"] = in.name;
        ij["type"] = in.type;
        ij["source"] = in.source;
        ij["denoise"] = in.denoise;
        ij["eqPreset"] = in.eqPreset;
        ij["spatial"] = in.spatial;
        ij["azimuth"] = in.azimuth;
        ij["elevation"] = in.elevation;
        ij["peak"] = in.peak;
        ij["rms"] = in.rms;
        j["inputs"].push_back(ij);
    }
    j["groups"] = nlohmann::json::array();
    for (const auto& g : s.groups) {
        nlohmann::json gj;
        gj["id"] = g.id;
        gj["name"] = g.name;
        gj["color"] = g.color;
        gj["cable"] = g.cable;
        gj["inputIds"] = g.inputIds;
        gj["outputIds"] = g.outputIds;
        gj["outputGains"] = g.outputGains;
        gj["volume"] = g.volume;
        gj["muted"] = g.muted;
        gj["peak"] = g.peak;
        gj["rms"] = g.rms;
        gj["knobIndex"] = g.knobIndex.has_value() ? nlohmann::json(*g.knobIndex) : nlohmann::json(nullptr);
        j["groups"].push_back(gj);
    }
    j["outputs"] = nlohmann::json::array();
    for (const auto& o : s.outputs) {
        nlohmann::json oj;
        oj["name"] = o.name;
        oj["master"] = o.master;
        oj["muted"] = o.muted;
        oj["groupIds"] = o.groupIds;
        oj["masterPeak"] = o.masterPeak;
        oj["masterRms"] = o.masterRms;
        j["outputs"].push_back(oj);
    }
    return j;
}

// Send gain of a group towards one output; unity when no entry exists.
float groupGainFor(const GroupConfig& g, const std::string& outName) {
    auto it = g.outputGains.find(outName);
    return it != g.outputGains.end() ? it->second : 1.0f;
}

InputProcessorConfig toInputProcessorConfig(const InputConfig& in) {
    InputProcessorConfig ic;
    ic.name   = in.name;
    ic.type   = in.type;
    ic.source = in.source;
    ic.denoise = in.denoise;
    ic.eqPreset = in.eqPreset;
    ic.spatial = in.spatial;
    ic.azimuth = in.azimuth;
    ic.elevation = in.elevation;
    return ic;
}

bool inputCfgNeedsRestart(const InputConfig& a, const InputConfig& b) {
    return a.type != b.type || a.source != b.source ||
           a.denoise != b.denoise || a.eqPreset != b.eqPreset ||
           a.spatial != b.spatial;
}

const char* palette[] = {
    "#3b82f6", "#ef4444", "#22c55e", "#eab308", "#a855f7",
    "#ec4899", "#06b6d4", "#f97316", "#84cc16", "#6366f1"
};
std::atomic<size_t> paletteIndex{0};

std::string nextColor() {
    size_t i = paletteIndex.fetch_add(1) % (sizeof(palette) / sizeof(palette[0]));
    return palette[i];
}

} // namespace

struct AudioMixerMatrix::Impl {
    struct InputEntry {
        InputConfig cfg;
        std::unique_ptr<InputProcessor> processor;
    };

    mutable std::mutex mtx;
    std::map<std::string, std::unique_ptr<OutputMixer>> outputs;
    std::map<InputId, InputEntry> inputs;
    std::map<GroupId, GroupConfig> groups;
    std::map<std::pair<GroupId, std::string>, std::shared_ptr<GroupBus>> groupBuses;

    std::atomic<InputId> nextInputId{1};
    std::atomic<GroupId> nextGroupId{1};

    ComPtr<IMMDeviceEnumerator> enumerator;

    OutputMixer* findOutputMixer(const std::string& name) const {
        auto it = outputs.find(name);
        return it != outputs.end() ? it->second.get() : nullptr;
    }

    bool ensureEnumerator() {
        if (enumerator) return true;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        return SUCCEEDED(hr);
    }

    InputProcessor* ensureInputProcessorLocked(InputId id, const InputConfig& cfg) {
        auto it = inputs.find(id);
        if (it == inputs.end()) return nullptr;

        // If the processor exists and the config still matches, just make sure
        // its HRTF direction is up to date (setDirection is a no-op when unchanged).
        if (it->second.processor) {
            if (inputCfgNeedsRestart(it->second.cfg, cfg)) {
                it->second.processor.reset();
            } else {
                it->second.processor->setDirection(cfg.azimuth, cfg.elevation);
                return it->second.processor.get();
            }
        }

        auto proc = std::make_unique<InputProcessor>();
        InputProcessorConfig ipc = toInputProcessorConfig(cfg);
        if (!proc->init(ipc)) {
            std::fprintf(stderr, "[AudioMixerMatrix] input '%s' (%s) failed to open\n",
                         cfg.name.c_str(), cfg.source.c_str());
            return nullptr;
        }
        if (!proc->start()) {
            std::fprintf(stderr, "[AudioMixerMatrix] input '%s' failed to start\n", cfg.name.c_str());
            return nullptr;
        }
        proc->setDirection(cfg.azimuth, cfg.elevation);
        it->second.cfg = cfg;
        it->second.processor = std::move(proc);
        return it->second.processor.get();
    }

    void stopInputProcessorLocked(InputId id) {
        auto it = inputs.find(id);
        if (it != inputs.end()) it->second.processor.reset();
    }

    void pruneUnusedInputProcessorsLocked() {
        std::set<InputId> used;
        for (const auto& kv : groupBuses) {
            for (InputProcessor* p : kv.second->inputProcessors()) {
                for (const auto& ik : inputs) {
                    if (ik.second.processor.get() == p) {
                        used.insert(ik.first);
                        break;
                    }
                }
            }
        }
        for (auto& kv : inputs) {
            if (!used.count(kv.first)) kv.second.processor.reset();
        }
    }

    void rebuildLocked();
};

AudioMixerMatrix::AudioMixerMatrix() : m_impl(std::make_unique<Impl>()) {}
AudioMixerMatrix::~AudioMixerMatrix() { stop(); }

bool AudioMixerMatrix::addOutput(const std::string& outputHint)
{
    auto out = std::make_unique<OutputMixer>();
    if (!out->init(outputHint)) return false;

    {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        m_impl->outputs[out->outputName()] = std::move(out);
        m_impl->rebuildLocked();
    }
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::removeOutput(const std::string& outputName)
{
    {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        auto it = m_impl->outputs.find(outputName);
        if (it == m_impl->outputs.end()) return false;

        it->second->stop();
        m_impl->outputs.erase(it);

        // Remove from all group output lists and drop any buses for this output.
        for (auto& gkv : m_impl->groups) {
            auto& outs = gkv.second.outputIds;
            outs.erase(std::remove(outs.begin(), outs.end(), outputName), outs.end());
        }
        m_impl->rebuildLocked();
    }
    maybeAutosave();
    return true;
}

std::vector<std::string> AudioMixerMatrix::outputNames() const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    std::vector<std::string> names;
    names.reserve(m_impl->outputs.size());
    for (const auto& kv : m_impl->outputs) names.push_back(kv.first);
    return names;
}

float AudioMixerMatrix::outputMasterVolume(const std::string& outputName) const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto* m = m_impl->findOutputMixer(outputName);
    return m ? linToPct(m->masterVolume()) : 100.0f;
}

float AudioMixerMatrix::outputMasterPeak(const std::string& outputName) const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto* m = m_impl->findOutputMixer(outputName);
    return m ? m->masterPeak() : 0.0f;
}

float AudioMixerMatrix::outputMasterRms(const std::string& outputName) const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto* m = m_impl->findOutputMixer(outputName);
    return m ? m->masterRms() : 0.0f;
}

void AudioMixerMatrix::setOutputMasterVolume(const std::string& outputName, float v)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto* m = m_impl->findOutputMixer(outputName);
    if (m) m->setMasterVolume(pctToLin(v));
    maybeAutosave();
}

std::optional<InputId> AudioMixerMatrix::addInput(const InputConfig& cfg)
{
    if (cfg.name.empty() || cfg.source.empty()) return std::nullopt;

    std::lock_guard<std::mutex> lk(m_impl->mtx);
    InputId id = m_impl->nextInputId.fetch_add(1);
    m_impl->inputs[id] = { cfg, nullptr };
    maybeAutosave();
    return id;
}

bool AudioMixerMatrix::removeInput(InputId id)
{
    {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        auto it = m_impl->inputs.find(id);
        if (it == m_impl->inputs.end()) return false;

        // Remove from all groups first so rebuildLocked() stops the processor
        // and removes it from all buses before we delete the entry.
        for (auto& gkv : m_impl->groups) {
            auto& ins = gkv.second.inputIds;
            ins.erase(std::remove(ins.begin(), ins.end(), id), ins.end());
        }

        m_impl->rebuildLocked();
        m_impl->inputs.erase(it);
    }
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::updateInput(InputId id, const InputConfig& cfg)
{
    if (cfg.name.empty() || cfg.source.empty()) return false;

    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->inputs.find(id);
    if (it == m_impl->inputs.end()) return false;

    if (inputCfgNeedsRestart(it->second.cfg, cfg)) {
        m_impl->stopInputProcessorLocked(id);
    }

    it->second.cfg = cfg;
    m_impl->rebuildLocked();
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::setInputDirection(InputId id, float azimuth, float elevation)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->inputs.find(id);
    if (it == m_impl->inputs.end()) return false;

    it->second.cfg.azimuth = azimuth;
    it->second.cfg.elevation = elevation;
    if (it->second.processor) {
        it->second.processor->setDirection(azimuth, elevation);
    }
    maybeAutosave();
    return true;
}

std::optional<GroupId> AudioMixerMatrix::addGroup(const GroupConfig& cfg)
{
    if (cfg.name.empty()) return std::nullopt;

    std::lock_guard<std::mutex> lk(m_impl->mtx);
    GroupId id = m_impl->nextGroupId.fetch_add(1);
    GroupConfig gc = cfg;
    if (gc.color.empty()) gc.color = nextColor();
    m_impl->groups[id] = std::move(gc);
    m_impl->rebuildLocked();
    maybeAutosave();
    return id;
}

bool AudioMixerMatrix::removeGroup(GroupId id)
{
    {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        auto it = m_impl->groups.find(id);
        if (it == m_impl->groups.end()) return false;

        m_impl->groups.erase(it);
        m_impl->rebuildLocked();
    }
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::setGroupName(GroupId id, const std::string& name)
{
    if (name.empty()) return false;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->groups.find(id);
    if (it == m_impl->groups.end()) return false;
    it->second.name = name;
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::setGroupColor(GroupId id, const std::string& color)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->groups.find(id);
    if (it == m_impl->groups.end()) return false;
    it->second.color = color;
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::setGroupCable(GroupId id, const std::string& cable)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->groups.find(id);
    if (it == m_impl->groups.end()) return false;
    it->second.cable = cable;
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::setGroupVolume(GroupId id, float vol)
{
    vol = pctToLin(vol);
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->groups.find(id);
    if (it == m_impl->groups.end()) return false;
    it->second.volume = vol;

    for (auto& kv : m_impl->groupBuses) {
        if (kv.first.first == id) {
            kv.second->setGain(vol * groupGainFor(it->second, kv.first.second));
        }
    }
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::setGroupMuted(GroupId id, bool muted)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->groups.find(id);
    if (it == m_impl->groups.end()) return false;
    it->second.muted = muted;

    for (auto& kv : m_impl->groupBuses) {
        if (kv.first.first == id) kv.second->setMuted(muted);
    }
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::setGroupOutputGain(GroupId id, const std::string& output, float gainPct)
{
    float gain = pctToLin(gainPct);
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->groups.find(id);
    if (it == m_impl->groups.end()) return false;

    if (gain == 1.0f) it->second.outputGains.erase(output);
    else it->second.outputGains[output] = gain;

    auto busIt = m_impl->groupBuses.find({ id, output });
    if (busIt != m_impl->groupBuses.end()) {
        busIt->second->setGain(it->second.volume * gain);
    }
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::setOutputMuted(const std::string& outputName, bool muted)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto* m = m_impl->findOutputMixer(outputName);
    if (!m) return false;
    m->setMasterMuted(muted);
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::outputMuted(const std::string& outputName) const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto* m = m_impl->findOutputMixer(outputName);
    return m != nullptr && m->masterMuted();
}

bool AudioMixerMatrix::setGroupKnobIndex(GroupId id, std::optional<int> knobIndex)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->groups.find(id);
    if (it == m_impl->groups.end()) return false;
    it->second.knobIndex = knobIndex;
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::setGroupInputIds(GroupId id, std::vector<InputId> ids)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->groups.find(id);
    if (it == m_impl->groups.end()) return false;

    for (InputId iid : ids) {
        if (m_impl->inputs.find(iid) == m_impl->inputs.end()) return false;
    }
    it->second.inputIds = std::move(ids);
    m_impl->rebuildLocked();
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::setGroupOutputIds(GroupId id, std::vector<std::string> ids)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->groups.find(id);
    if (it == m_impl->groups.end()) return false;

    for (const auto& n : ids) {
        if (m_impl->outputs.find(n) == m_impl->outputs.end()) return false;
    }
    it->second.outputIds = std::move(ids);
    m_impl->rebuildLocked();
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::addGroupInput(GroupId groupId, InputId inputId)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto git = m_impl->groups.find(groupId);
    if (git == m_impl->groups.end()) return false;
    if (m_impl->inputs.find(inputId) == m_impl->inputs.end()) return false;

    auto& ids = git->second.inputIds;
    if (std::find(ids.begin(), ids.end(), inputId) != ids.end()) return true;
    ids.push_back(inputId);
    m_impl->rebuildLocked();
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::removeGroupInput(GroupId groupId, InputId inputId)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto git = m_impl->groups.find(groupId);
    if (git == m_impl->groups.end()) return false;

    auto& ids = git->second.inputIds;
    ids.erase(std::remove(ids.begin(), ids.end(), inputId), ids.end());
    m_impl->rebuildLocked();
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::addGroupOutput(GroupId groupId, const std::string& outputName)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto git = m_impl->groups.find(groupId);
    if (git == m_impl->groups.end()) return false;
    if (m_impl->outputs.find(outputName) == m_impl->outputs.end()) return false;

    auto& ids = git->second.outputIds;
    if (std::find(ids.begin(), ids.end(), outputName) != ids.end()) return true;
    ids.push_back(outputName);
    m_impl->rebuildLocked();
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::removeGroupOutput(GroupId groupId, const std::string& outputName)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto git = m_impl->groups.find(groupId);
    if (git == m_impl->groups.end()) return false;

    auto& ids = git->second.outputIds;
    ids.erase(std::remove(ids.begin(), ids.end(), outputName), ids.end());
    m_impl->rebuildLocked();
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::start()
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->rebuildLocked();
    bool any = false;
    for (auto& kv : m_impl->outputs) {
        if (!kv.second->running()) {
            if (kv.second->start()) any = true;
        } else {
            any = true;
        }
    }
    return any;
}

void AudioMixerMatrix::stop()
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    for (auto& kv : m_impl->outputs) kv.second->stop();
    for (auto& kv : m_impl->inputs) kv.second.processor.reset();
}

bool AudioMixerMatrix::running() const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    for (const auto& kv : m_impl->outputs) {
        if (kv.second->running()) return true;
    }
    return false;
}

MixerStateSnapshot AudioMixerMatrix::snapshotNoLock() const
{
    MixerStateSnapshot s;
    s.controlPort = m_controlPort;

    for (const auto& kv : m_impl->inputs) {
        InputSnapshot in;
        in.id = kv.first;
        in.name = kv.second.cfg.name;
        in.type = kv.second.cfg.type;
        in.source = kv.second.cfg.source;
        in.denoise = kv.second.cfg.denoise;
        in.eqPreset = kv.second.cfg.eqPreset;
        in.spatial = kv.second.cfg.spatial;
        in.azimuth = kv.second.cfg.azimuth;
        in.elevation = kv.second.cfg.elevation;
        if (kv.second.processor) {
            in.peak = kv.second.processor->peak();
            in.rms = kv.second.processor->rms();
        }
        s.inputs.push_back(in);
    }
    std::sort(s.inputs.begin(), s.inputs.end(), [](const InputSnapshot& a, const InputSnapshot& b) { return a.id < b.id; });

    for (const auto& kv : m_impl->groups) {
        GroupSnapshot g;
        g.id = kv.first;
        g.name = kv.second.name;
        g.color = kv.second.color;
        g.cable = kv.second.cable;
        g.inputIds = kv.second.inputIds;
        g.outputIds = kv.second.outputIds;
        for (const auto& gk : kv.second.outputGains) g.outputGains[gk.first] = linToPct(gk.second);
        g.volume = linToPct(kv.second.volume);
        g.muted = kv.second.muted;
        g.knobIndex = kv.second.knobIndex;

        for (const auto& bkv : m_impl->groupBuses) {
            if (bkv.first.first == kv.first) {
                g.peak = std::max(g.peak, bkv.second->peak());
                g.rms = std::max(g.rms, bkv.second->rms());
            }
        }
        s.groups.push_back(g);
    }
    std::sort(s.groups.begin(), s.groups.end(), [](const GroupSnapshot& a, const GroupSnapshot& b) { return a.id < b.id; });

    for (const auto& kv : m_impl->outputs) {
        OutputSnapshot o;
        o.name = kv.first;
        o.master = linToPct(kv.second->masterVolume());
        o.muted = kv.second->masterMuted();
        o.masterPeak = kv.second->masterPeak();
        o.masterRms = kv.second->masterRms();
        s.outputs.push_back(o);
    }
    std::sort(s.outputs.begin(), s.outputs.end(), [](const OutputSnapshot& a, const OutputSnapshot& b) { return a.name < b.name; });

    for (auto& o : s.outputs) {
        for (const auto& g : s.groups) {
            if (std::find(g.outputIds.begin(), g.outputIds.end(), o.name) != g.outputIds.end()) {
                o.groupIds.push_back(g.id);
            }
        }
    }

    s.running = false;
    for (const auto& kv : m_impl->outputs) {
        if (kv.second->running()) { s.running = true; break; }
    }
    return s;
}

MixerStateSnapshot AudioMixerMatrix::snapshot() const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    return snapshotNoLock();
}

std::vector<EndpointInfo> AudioMixerMatrix::listEndpoints() const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    if (!m_impl->ensureEnumerator()) return {};
    return enumEndpoints(m_impl->enumerator.Get());
}

std::vector<ApplicationInfo> AudioMixerMatrix::listApplications() const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    if (!m_impl->ensureEnumerator()) return {};
    return enumAudioSessions(m_impl->enumerator.Get());
}

bool AudioMixerMatrix::saveSnapshot(const std::string& path, const MixerStateSnapshot& s) const
{
    if (path.empty()) return false;
    try {
        std::ofstream f(path);
        if (!f) return false;
        f << toJson(s).dump(2);
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[AudioMixerMatrix] save failed: %s\n", e.what());
        return false;
    }
}

bool AudioMixerMatrix::save(const std::string& path) const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    return saveSnapshot(path, snapshotNoLock());
}

bool AudioMixerMatrix::load(const std::string& path)
{
    std::ifstream f(path);
    if (!f) return false;

    nlohmann::json j;
    try { f >> j; } catch (...) { return false; }

    stop();

    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_controlPort = j.value("controlPort", m_controlPort);
    m_impl->inputs.clear();
    m_impl->groups.clear();
    m_impl->groupBuses.clear();
    m_impl->outputs.clear();

    // Legacy: single output + strips.
    if (j.contains("output") && j["output"].is_string()) {
        std::string outName = j["output"].get<std::string>();
        float master = j.value("master", 100.0f) / 100.0f;
        if (addOutputLocked(outName)) {
            auto* m = m_impl->findOutputMixer(outName);
            if (m) m->setMasterVolume(master);
        }
        if (j.contains("strips") && j["strips"].is_array()) {
            for (const auto& item : j["strips"]) {
                InputConfig ic;
                ic.name = item.value("name", std::string{});
                ic.source = item.value("source", std::string{});
                if (ic.source.empty()) continue;
                if (ic.name.empty()) ic.name = ic.source;
                ic.type = "device";
                InputId iid = m_impl->nextInputId.fetch_add(1);
                m_impl->inputs[iid] = { ic, nullptr };

                GroupConfig gc;
                gc.name = item.value("name", ic.name);
                gc.volume = item.value("volume", 100.0f) / 100.0f;
                gc.muted = item.value("muted", false);
                gc.inputIds = { iid };
                gc.outputIds = { outName };
                if (item.contains("knobIndex") && !item["knobIndex"].is_null()) {
                    gc.knobIndex = item["knobIndex"].get<int>();
                }
                GroupId gid = m_impl->nextGroupId.fetch_add(1);
                m_impl->groups[gid] = std::move(gc);
            }
        }
    }
    // Legacy: outputs + routes.
    else if (j.contains("routes") && j["routes"].is_array()) {
        if (j.contains("outputs") && j["outputs"].is_array()) {
            for (const auto& o : j["outputs"]) {
                std::string name = o.is_string() ? o.get<std::string>() : o.value("name", std::string{});
                float master = o.is_object() ? o.value("master", 100.0f) / 100.0f : 1.0f;
                if (!name.empty() && addOutputLocked(name)) {
                    auto* m = m_impl->findOutputMixer(name);
                    if (m) m->setMasterVolume(master);
                }
            }
        } else if (j.contains("output") && j["output"].is_string()) {
            std::string name = j["output"].get<std::string>();
            float master = j.value("master", 100.0f) / 100.0f;
            if (addOutputLocked(name)) {
                auto* m = m_impl->findOutputMixer(name);
                if (m) m->setMasterVolume(master);
            }
        }

        for (const auto& r : j["routes"]) {
            InputConfig ic;
            ic.name = r.value("name", std::string{});
            ic.source = r.value("source", std::string{});
            if (ic.source.empty()) continue;
            if (ic.name.empty()) ic.name = ic.source;
            ic.type = "device";
            InputId iid = m_impl->nextInputId.fetch_add(1);
            m_impl->inputs[iid] = { ic, nullptr };

            std::string outName = r.value("output", std::string{});
            if (outName.empty() && j.contains("output") && j["output"].is_string()) {
                outName = j["output"].get<std::string>();
            }

            GroupConfig gc;
            gc.name = r.value("name", ic.name);
            gc.color = nextColor();
            gc.volume = r.value("volume", 100.0f) / 100.0f;
            gc.muted = r.value("muted", false);
            gc.inputIds = { iid };
            gc.outputIds = { outName };
            if (r.contains("knobIndex") && !r["knobIndex"].is_null()) {
                gc.knobIndex = r["knobIndex"].get<int>();
            }
            GroupId gid = m_impl->nextGroupId.fetch_add(1);
            m_impl->groups[gid] = std::move(gc);
        }
    }
    // New: inputs + groups + outputs.
    else {
        if (j.contains("outputs") && j["outputs"].is_array()) {
            for (const auto& o : j["outputs"]) {
                std::string name = o.is_string() ? o.get<std::string>() : o.value("name", std::string{});
                float master = o.is_object() ? o.value("master", 100.0f) / 100.0f : 1.0f;
                bool muted = o.is_object() && o.value("muted", false);
                if (!name.empty() && addOutputLocked(name)) {
                    auto* m = m_impl->findOutputMixer(name);
                    if (m) {
                        m->setMasterVolume(master);
                        m->setMasterMuted(muted);
                    }
                }
            }
        }

        if (j.contains("inputs") && j["inputs"].is_array()) {
            for (const auto& i : j["inputs"]) {
                InputConfig ic;
                ic.name = i.value("name", std::string{});
                ic.type = i.value("type", "device");
                ic.source = i.value("source", std::string{});
                ic.denoise = i.value("denoise", false);
                ic.eqPreset = i.value("eqPreset", std::string{});
                ic.spatial = i.value("spatial", false);
                ic.azimuth = i.value("azimuth", 0.0f);
                ic.elevation = i.value("elevation", 0.0f);
                if (ic.source.empty()) continue;
                if (ic.name.empty()) ic.name = ic.source;
                InputId iid = i.value("id", 0);
                if (iid == 0) iid = m_impl->nextInputId.fetch_add(1);
                else m_impl->nextInputId.store(std::max(m_impl->nextInputId.load(), (InputId)(iid + 1)));
                m_impl->inputs[iid] = { ic, nullptr };
            }
        }

        if (j.contains("groups") && j["groups"].is_array()) {
            for (const auto& g : j["groups"]) {
                GroupConfig gc;
                gc.name = g.value("name", std::string{});
                gc.color = g.value("color", std::string{"#3b82f6"});
                gc.cable = g.value("cable", std::string{});
                gc.volume = g.value("volume", 100.0f) / 100.0f;
                gc.muted = g.value("muted", false);
                if (g.contains("inputIds")) gc.inputIds = g["inputIds"].get<std::vector<InputId>>();
                if (g.contains("outputIds")) gc.outputIds = g["outputIds"].get<std::vector<std::string>>();
                if (g.contains("outputGains") && g["outputGains"].is_object()) {
                    for (const auto& kv : g["outputGains"].items()) {
                        gc.outputGains[kv.key()] = kv.value().get<float>() / 100.0f;
                    }
                }
                if (g.contains("knobIndex") && !g["knobIndex"].is_null()) {
                    gc.knobIndex = g["knobIndex"].get<int>();
                }
                if (gc.color.empty()) gc.color = nextColor();
                if (gc.name.empty()) continue;
                GroupId gid = g.value("id", 0);
                if (gid == 0) gid = m_impl->nextGroupId.fetch_add(1);
                else m_impl->nextGroupId.store(std::max(m_impl->nextGroupId.load(), (GroupId)(gid + 1)));
                m_impl->groups[gid] = std::move(gc);
            }
        }
    }

    m_impl->rebuildLocked();
    for (auto& kv : m_impl->outputs) {
        if (!kv.second->running()) kv.second->start();
    }
    return true;
}

void AudioMixerMatrix::setAutosavePath(const std::string& path)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_autosavePath = path;
}

void AudioMixerMatrix::setControlPort(uint16_t port)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_controlPort = port;
}

bool AudioMixerMatrix::autosaveEnabled() const
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    return !m_autosavePath.empty();
}

void AudioMixerMatrix::maybeAutosave() const
{
    if (!m_autosavePath.empty()) {
        saveSnapshot(m_autosavePath, snapshotNoLock());
    }
}

void AudioMixerMatrix::Impl::rebuildLocked()
{
    // Build the desired set of (group, output) buses.
    std::set<std::pair<GroupId, std::string>> desired;
    for (const auto& gkv : groups) {
        for (const auto& outName : gkv.second.outputIds) {
            if (outputs.find(outName) != outputs.end()) desired.insert({ gkv.first, outName });
        }
    }

    // Drop group buses that are no longer desired.
    for (auto it = groupBuses.begin(); it != groupBuses.end(); ) {
        if (!desired.count(it->first)) {
            auto outIt = outputs.find(it->first.second);
            if (outIt != outputs.end()) outIt->second->removeGroupBus(it->second.get());
            it = groupBuses.erase(it);
        } else {
            ++it;
        }
    }

    // Create or update each desired group bus.
    for (const auto& key : desired) {
        auto git = groups.find(key.first);
        auto outIt = outputs.find(key.second);
        if (git == groups.end() || outIt == outputs.end()) continue;
        const GroupConfig& g = git->second;

        std::shared_ptr<GroupBus> bus;
        auto it = groupBuses.find(key);
        if (it == groupBuses.end()) {
            bus = std::make_shared<GroupBus>();
            groupBuses[key] = bus;
            outIt->second->addGroupBus(bus);
        } else {
            bus = it->second;
        }

        bus->setGain(g.volume * groupGainFor(g, key.second));
        bus->setMuted(g.muted);

        // Determine the set of InputProcessor pointers this bus should mix.
        std::set<InputProcessor*> desiredProcs;
        for (InputId iid : g.inputIds) {
            auto iit = inputs.find(iid);
            if (iit == inputs.end()) continue;
            InputProcessor* proc = ensureInputProcessorLocked(iid, iit->second.cfg);
            if (proc) desiredProcs.insert(proc);
        }

        // Remove inputs no longer in the group.
        for (InputProcessor* p : bus->inputProcessors()) {
            if (!desiredProcs.count(p)) bus->removeInput(*p);
        }

        // Add new inputs.
        for (InputProcessor* p : desiredProcs) {
            bus->addInput(*p);
        }
    }

    pruneUnusedInputProcessorsLocked();

    // Stop outputs that no longer have buses; start outputs that do.
    for (auto& kv : outputs) {
        bool hasBus = false;
        for (const auto& bkv : groupBuses) {
            if (bkv.first.second == kv.first) { hasBus = true; break; }
        }
        if (hasBus) {
            if (!kv.second->running()) kv.second->start();
        } else {
            if (kv.second->running()) kv.second->stop();
        }
    }
}

bool AudioMixerMatrix::addOutputLocked(const std::string& outputHint)
{
    auto out = std::make_unique<OutputMixer>();
    if (!out->init(outputHint)) return false;
    m_impl->outputs[out->outputName()] = std::move(out);
    return true;
}

} // namespace anniaudio::core
