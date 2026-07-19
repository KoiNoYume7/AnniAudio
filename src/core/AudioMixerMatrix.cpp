#include "AudioMixerMatrix.hpp"
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
        gj["inputIds"] = g.inputIds;
        gj["outputIds"] = g.outputIds;
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
        oj["groupIds"] = o.groupIds;
        oj["masterPeak"] = o.masterPeak;
        oj["masterRms"] = o.masterRms;
        j["outputs"].push_back(oj);
    }
    return j;
}

MixerStripConfig stripForRoute(const InputConfig& in, const GroupConfig& g) {
    MixerStripConfig cfg;
    cfg.name = g.name;
    cfg.source = in.source;
    cfg.volume = g.volume;
    cfg.muted = g.muted;
    cfg.knobIndex = g.knobIndex;
    return cfg;
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
    struct RouteInfo {
        InputId     inputId = 0;
        GroupId     groupId = 0;
        std::string output;
        AudioMixer* mixer = nullptr;
        StripId     localId = 0;
    };

    mutable std::mutex mtx;
    std::map<std::string, std::unique_ptr<AudioMixer>> outputs;
    std::map<InputId, InputConfig> inputs;
    std::map<GroupId, GroupConfig> groups;
    std::unordered_map<RouteId, RouteInfo> routes;
    std::map<std::pair<AudioMixer*, StripId>, RouteId> localToGlobal;

    std::atomic<InputId> nextInputId{1};
    std::atomic<GroupId> nextGroupId{1};
    std::atomic<RouteId> nextRouteId{1};

    ComPtr<IMMDeviceEnumerator> enumerator;

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

    void eraseRoutesFor(std::function<bool(const RouteInfo&)> pred) {
        for (auto it = routes.begin(); it != routes.end(); ) {
            if (pred(it->second)) {
                if (it->second.mixer) {
                    it->second.mixer->removeStrip(it->second.localId);
                    localToGlobal.erase({ it->second.mixer, it->second.localId });
                }
                it = routes.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Stop an output mixer when it has no strips.
    void pruneEmptyOutputs() {
        for (auto& kv : outputs) {
            if (kv.second->stripCount() == 0 && kv.second->running()) {
                kv.second->stop();
            }
        }
    }
};

AudioMixerMatrix::AudioMixerMatrix() : m_impl(std::make_unique<Impl>()) {}
AudioMixerMatrix::~AudioMixerMatrix() { stop(); }

bool AudioMixerMatrix::addOutput(const std::string& outputHint)
{
    auto mixer = std::make_unique<AudioMixer>();
    if (!mixer->init(outputHint)) return false;

    {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        m_impl->outputs[mixer->outputName()] = std::move(mixer);
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
        m_impl->eraseRoutesFor([outputName](const Impl::RouteInfo& r) { return r.output == outputName; });
        m_impl->outputs.erase(it);

        for (auto& gkv : m_impl->groups) {
            auto& outs = gkv.second.outputIds;
            outs.erase(std::remove(outs.begin(), outs.end(), outputName), outs.end());
        }
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
    m_impl->inputs[id] = cfg;
    maybeAutosave();
    return id;
}

bool AudioMixerMatrix::removeInput(InputId id)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->inputs.find(id);
    if (it == m_impl->inputs.end()) return false;

    m_impl->eraseRoutesFor([id](const Impl::RouteInfo& r) { return r.inputId == id; });

    // Remove from all groups.
    for (auto& gkv : m_impl->groups) {
        auto& ins = gkv.second.inputIds;
        ins.erase(std::remove(ins.begin(), ins.end(), id), ins.end());
    }

    m_impl->inputs.erase(it);
    m_impl->pruneEmptyOutputs();
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::updateInput(InputId id, const InputConfig& cfg)
{
    if (cfg.name.empty() || cfg.source.empty()) return false;

    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->inputs.find(id);
    if (it == m_impl->inputs.end()) return false;

    it->second = cfg;

    // Rebuild routes for all groups that use this input.
    std::set<GroupId> affected;
    for (const auto& kv : m_impl->routes) {
        if (kv.second.inputId == id) affected.insert(kv.second.groupId);
    }
    for (GroupId gid : affected) rebuildGroupRoutesLocked(gid);

    maybeAutosave();
    return true;
}

std::optional<GroupId> AudioMixerMatrix::addGroup(const GroupConfig& cfg)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    GroupId id = m_impl->nextGroupId.fetch_add(1);
    GroupConfig gc = cfg;
    if (gc.color.empty()) gc.color = nextColor();
    m_impl->groups[id] = std::move(gc);
    rebuildGroupRoutesLocked(id);
    maybeAutosave();
    return id;
}

bool AudioMixerMatrix::removeGroup(GroupId id)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->groups.find(id);
    if (it == m_impl->groups.end()) return false;

    m_impl->eraseRoutesFor([id](const Impl::RouteInfo& r) { return r.groupId == id; });
    m_impl->groups.erase(it);
    m_impl->pruneEmptyOutputs();
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

    for (auto& kv : m_impl->routes) {
        if (kv.second.groupId == id) {
            kv.second.mixer->renameStrip(kv.second.localId, name);
        }
    }
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

bool AudioMixerMatrix::setGroupVolume(GroupId id, float vol)
{
    vol = pctToLin(vol);
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->groups.find(id);
    if (it == m_impl->groups.end()) return false;
    it->second.volume = vol;

    for (const auto& kv : m_impl->routes) {
        if (kv.second.groupId == id) {
            kv.second.mixer->setStripVolume(kv.second.localId, vol);
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

    for (const auto& kv : m_impl->routes) {
        if (kv.second.groupId == id) {
            kv.second.mixer->setStripMuted(kv.second.localId, muted);
        }
    }
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::setGroupKnobIndex(GroupId id, std::optional<int> knobIndex)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->groups.find(id);
    if (it == m_impl->groups.end()) return false;
    it->second.knobIndex = knobIndex;
    for (const auto& kv : m_impl->routes) {
        if (kv.second.groupId == id) {
            kv.second.mixer->setStripKnobIndex(kv.second.localId, knobIndex);
        }
    }
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::setGroupInputIds(GroupId id, std::vector<InputId> ids)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->groups.find(id);
    if (it == m_impl->groups.end()) return false;

    // Validate inputs exist.
    for (InputId iid : ids) {
        if (m_impl->inputs.find(iid) == m_impl->inputs.end()) return false;
    }
    it->second.inputIds = std::move(ids);
    if (!rebuildGroupRoutesLocked(id)) {
        // If rebuild failed (e.g. no routes possible), leave empty.
    }
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::setGroupOutputIds(GroupId id, std::vector<std::string> ids)
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    auto it = m_impl->groups.find(id);
    if (it == m_impl->groups.end()) return false;

    // Validate outputs exist.
    for (const auto& n : ids) {
        if (m_impl->outputs.find(n) == m_impl->outputs.end()) return false;
    }
    it->second.outputIds = std::move(ids);
    if (!rebuildGroupRoutesLocked(id)) {
        // Leave empty if nothing to route.
    }
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
    rebuildGroupRoutesLocked(groupId);
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
    rebuildGroupRoutesLocked(groupId);
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
    rebuildGroupRoutesLocked(groupId);
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
    rebuildGroupRoutesLocked(groupId);
    maybeAutosave();
    return true;
}

bool AudioMixerMatrix::start()
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    bool any = false;
    for (auto& kv : m_impl->outputs) {
        if (kv.second->start()) any = true;
    }
    return any;
}

void AudioMixerMatrix::stop()
{
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    for (auto& kv : m_impl->outputs) kv.second->stop();
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

    // Inputs
    for (const auto& kv : m_impl->inputs) {
        InputSnapshot in;
        in.id = kv.first;
        in.name = kv.second.name;
        in.type = kv.second.type;
        in.source = kv.second.source;
        s.inputs.push_back(in);
    }
    std::sort(s.inputs.begin(), s.inputs.end(), [](const InputSnapshot& a, const InputSnapshot& b) { return a.id < b.id; });

    // Groups
    for (const auto& kv : m_impl->groups) {
        GroupSnapshot g;
        g.id = kv.first;
        g.name = kv.second.name;
        g.color = kv.second.color;
        g.inputIds = kv.second.inputIds;
        g.outputIds = kv.second.outputIds;
        g.volume = linToPct(kv.second.volume);
        g.muted = kv.second.muted;
        g.knobIndex = kv.second.knobIndex;
        s.groups.push_back(g);
    }
    std::sort(s.groups.begin(), s.groups.end(), [](const GroupSnapshot& a, const GroupSnapshot& b) { return a.id < b.id; });

    // Outputs
    for (const auto& kv : m_impl->outputs) {
        OutputSnapshot o;
        o.name = kv.first;
        o.master = linToPct(kv.second->masterVolume());
        o.masterPeak = kv.second->masterPeak();
        o.masterRms = kv.second->masterRms();
        s.outputs.push_back(o);
    }
    std::sort(s.outputs.begin(), s.outputs.end(), [](const OutputSnapshot& a, const OutputSnapshot& b) { return a.name < b.name; });

    // Aggregate route levels into inputs and groups.
    std::map<GroupId, std::vector<std::pair<float,float>>> groupLevels;
    std::map<InputId, std::vector<std::pair<float,float>>> inputLevels;

    for (const auto& kv : m_impl->routes) {
        const auto& r = kv.second;
        auto snap = r.mixer->stripSnapshot(r.localId);
        if (!snap) continue;
        groupLevels[r.groupId].push_back({ snap->peak, snap->rms });
        inputLevels[r.inputId].push_back({ snap->peak, snap->rms });
    }

    auto maxPair = [](const std::vector<std::pair<float,float>>& v, std::pair<float,float>& out) {
        out = {0.0f, 0.0f};
        for (const auto& p : v) {
            out.first = std::max(out.first, p.first);
            out.second = std::max(out.second, p.second);
        }
    };

    std::pair<float,float> p;
    for (auto& g : s.groups) {
        auto it = groupLevels.find(g.id);
        if (it != groupLevels.end()) {
            maxPair(it->second, p);
            g.peak = p.first;
            g.rms = p.second;
        }
    }
    for (auto& in : s.inputs) {
        auto it = inputLevels.find(in.id);
        if (it != inputLevels.end()) {
            maxPair(it->second, p);
            in.peak = p.first;
            in.rms = p.second;
        }
    }

    // Build output groupIds from groups referencing each output.
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
    m_impl->routes.clear();
    m_impl->localToGlobal.clear();
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
                m_impl->inputs[iid] = ic;

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
            m_impl->inputs[iid] = ic;

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
                if (!name.empty() && addOutputLocked(name)) {
                    auto* m = m_impl->findOutputMixer(name);
                    if (m) m->setMasterVolume(master);
                }
            }
        }

        if (j.contains("inputs") && j["inputs"].is_array()) {
            for (const auto& i : j["inputs"]) {
                InputConfig ic;
                ic.name = i.value("name", std::string{});
                ic.type = i.value("type", "device");
                ic.source = i.value("source", std::string{});
                if (ic.source.empty()) continue;
                if (ic.name.empty()) ic.name = ic.source;
                InputId iid = i.value("id", 0);
                if (iid == 0) iid = m_impl->nextInputId.fetch_add(1);
                else m_impl->nextInputId.store(std::max(m_impl->nextInputId.load(), (InputId)(iid + 1)));
                m_impl->inputs[iid] = ic;
            }
        }

        if (j.contains("groups") && j["groups"].is_array()) {
            for (const auto& g : j["groups"]) {
                GroupConfig gc;
                gc.name = g.value("name", std::string{});
                gc.color = g.value("color", std::string{"#3b82f6"});
                gc.volume = g.value("volume", 100.0f) / 100.0f;
                gc.muted = g.value("muted", false);
                if (g.contains("inputIds")) gc.inputIds = g["inputIds"].get<std::vector<InputId>>();
                if (g.contains("outputIds")) gc.outputIds = g["outputIds"].get<std::vector<std::string>>();
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

    // Rebuild routes for all groups.
    std::vector<GroupId> gids;
    for (const auto& kv : m_impl->groups) gids.push_back(kv.first);
    for (GroupId gid : gids) rebuildGroupRoutesLocked(gid);

    // Start any output that has strips.
    for (auto& kv : m_impl->outputs) {
        if (!kv.second->running() && kv.second->stripCount() > 0) kv.second->start();
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

// Rebuild all AudioMixer strips for a group. Must be called with mtx held.
bool AudioMixerMatrix::rebuildGroupRoutesLocked(GroupId id)
{
    auto git = m_impl->groups.find(id);
    if (git == m_impl->groups.end()) return false;
    const auto& g = git->second;

    // Remove existing routes for this group.
    m_impl->eraseRoutesFor([id](const Impl::RouteInfo& r) { return r.groupId == id; });

    bool any = false;
    for (InputId iid : g.inputIds) {
        auto iit = m_impl->inputs.find(iid);
        if (iit == m_impl->inputs.end()) continue;

        for (const auto& outName : g.outputIds) {
            AudioMixer* mixer = m_impl->findOutputMixer(outName);
            if (!mixer) continue;

            MixerStripConfig cfg = stripForRoute(iit->second, g);
            auto local = mixer->addStrip(cfg);
            if (!local) {
                std::fprintf(stderr, "[AudioMixerMatrix] failed to open input '%s' for group '%s' -> '%s'\n",
                             iit->second.source.c_str(), g.name.c_str(), outName.c_str());
                continue;
            }

            RouteId rid = m_impl->nextRouteId.fetch_add(1);
            m_impl->routes[rid] = { iid, id, outName, mixer, *local };
            m_impl->localToGlobal[{ mixer, *local }] = rid;
            if (!mixer->running() && mixer->stripCount() > 0) mixer->start();
            any = true;
        }
    }

    m_impl->pruneEmptyOutputs();
    return any;
}

// Used during load before start(); must be called with mtx held.
bool AudioMixerMatrix::addOutputLocked(const std::string& outputHint)
{
    auto mixer = std::make_unique<AudioMixer>();
    if (!mixer->init(outputHint)) return false;
    m_impl->outputs[mixer->outputName()] = std::move(mixer);
    return true;
}

} // namespace anniaudio::core
