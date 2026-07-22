#include "MixerControlServer.hpp"
#include "AudioMixerMatrix.hpp"

#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <windows.h>

#include <httplib/httplib.h>
#include <nlohmann/json.hpp>

namespace anniaudio::core {

namespace {

void EnsureComInitializedOnThisThread()
{
    thread_local bool initialized = false;
    if (initialized) return;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    initialized = true;
}

nlohmann::json toJson(const InputSnapshot& in)
{
    nlohmann::json j;
    j["id"]       = in.id;
    j["name"]     = in.name;
    j["type"]     = in.type;
    j["source"]   = in.source;
    j["denoise"]  = in.denoise;
    j["eqPreset"] = in.eqPreset;
    j["spatial"]  = in.spatial;
    j["azimuth"]  = in.azimuth;
    j["elevation"]= in.elevation;
    j["peak"]     = in.peak;
    j["rms"]      = in.rms;
    return j;
}

nlohmann::json toJson(const GroupSnapshot& g)
{
    nlohmann::json j;
    j["id"]        = g.id;
    j["name"]      = g.name;
    j["color"]     = g.color;
    j["cable"]     = g.cable;
    j["inputIds"]  = g.inputIds;
    j["outputIds"] = g.outputIds;
    j["outputGains"] = g.outputGains;
    j["volume"]    = g.volume;
    j["muted"]     = g.muted;
    j["peak"]      = g.peak;
    j["rms"]       = g.rms;
    j["knobIndex"] = g.knobIndex.has_value() ? nlohmann::json(*g.knobIndex) : nlohmann::json(nullptr);
    return j;
}

nlohmann::json toJson(const OutputSnapshot& o)
{
    nlohmann::json j;
    j["name"]        = o.name;
    j["master"]      = o.master;
    j["muted"]       = o.muted;
    j["groupIds"]    = o.groupIds;
    j["masterPeak"]  = o.masterPeak;
    j["masterRms"]   = o.masterRms;
    return j;
}

nlohmann::json toJson(const EndpointInfo& e)
{
    nlohmann::json j;
    j["id"]          = e.id;
    j["name"]        = e.name;
    j["isRender"]    = e.isRender;
    j["isDefault"]   = e.isDefault;
    j["isAnniAudio"] = e.isAnniAudio;
    return j;
}

nlohmann::json toJson(const ApplicationInfo& a)
{
    nlohmann::json j;
    j["processId"]   = a.processId;
    j["name"]        = a.name;
    j["displayName"] = a.displayName;
    j["windowTitle"] = a.windowTitle;
    j["endpoint"]    = a.endpoint;
    j["isInput"]     = a.isInput;
    j["isActive"]    = a.isActive;
    j["isMuted"]     = a.isMuted;
    j["volume"]      = a.volume;
    j["isSystem"]    = a.isSystem;
    return j;
}

nlohmann::json stateJson(AudioMixerMatrix& matrix)
{
    auto snap = matrix.snapshot();
    nlohmann::json j;
    j["running"] = snap.running;
    j["inputs"]  = nlohmann::json::array();
    j["groups"]  = nlohmann::json::array();
    j["outputs"] = nlohmann::json::array();
    for (const auto& in : snap.inputs) j["inputs"].push_back(toJson(in));
    for (const auto& g  : snap.groups) j["groups"].push_back(toJson(g));
    for (const auto& o  : snap.outputs) j["outputs"].push_back(toJson(o));
    return j;
}

void sendJson(httplib::Response& res, const nlohmann::json& j, int status = 200)
{
    res.status = status;
    res.set_content(j.dump(), "application/json");
}

void sendError(httplib::Response& res, int status, const std::string& message)
{
    sendJson(res, nlohmann::json{ { "error", message } }, status);
}

bool isSafePresetPath(const std::string& path)
{
    if (path.empty()) return false;
    if (path.find("..") != std::string::npos) return false;
    if (path.size() > 1 && (path[0] == '/' || path[0] == '\\')) return false;
    if (path.size() > 2 && path[1] == ':') return false;
    return true;
}

// Scene names become file names; keep them to a boring safe alphabet.
bool isSafeSceneName(const std::string& name)
{
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

std::optional<int> parseOptionalInt(const nlohmann::json& j, const std::string& key)
{
    if (!j.contains(key)) return std::nullopt;
    if (j[key].is_null()) return std::nullopt;
    return j[key].get<int>();
}

} // namespace

struct MixerControlServer::Impl {
    AudioMixerMatrix& matrix;
    std::string autosavePath;

    httplib::Server svr;
    std::thread listenThread;
    std::thread broadcastThread;

    struct SseClient {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::string> queue;
        bool closed = false;
    };
    std::mutex clientsMutex;
    std::unordered_map<uint64_t, std::shared_ptr<SseClient>> clients;
    std::atomic<uint64_t> nextClientId{1};

    std::mutex dirtyMutex;
    std::condition_variable dirtyCv;
    bool dirty = true;
    std::string lastBroadcastState;

    explicit Impl(AudioMixerMatrix& m) : matrix(m) {}

    void setAutosavePath(const std::string& path) {
        autosavePath = path;
        matrix.setAutosavePath(path);
    }

    // Scenes live in config/scenes/ next to config/mixers/<autosave>.json.
    // A scene is a lightweight level overlay (volumes, mutes, send gains,
    // output masters) applied by NAME, never a topology change, so applying
    // one is instant and glitch-free.
    std::string sceneDir() const
    {
        namespace fs = std::filesystem;
        if (autosavePath.empty()) return "config/scenes";
        fs::path parent = fs::path(autosavePath).parent_path().parent_path();
        if (parent.empty()) return "scenes";
        return (parent / "scenes").string();
    }

    std::string scenePath(const std::string& name) const
    {
        return (std::filesystem::path(this->sceneDir()) / (name + ".json")).string();
    }

    void markDirty()
    {
        std::lock_guard<std::mutex> lk(dirtyMutex);
        dirty = true;
        dirtyCv.notify_all();
    }

    void pushToAllClients(const std::string& chunk)
    {
        std::lock_guard<std::mutex> lk(clientsMutex);
        for (auto& [id, client] : clients) {
            std::lock_guard<std::mutex> clk(client->mutex);
            client->queue.push_back(chunk);
            client->cv.notify_all();
        }
    }

    void broadcastLoop()
    {
        // Wait for the listen thread to actually start, otherwise we can exit
        // immediately because svr.is_running() is not yet true.
        for (int i = 0; i < 500 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        while (svr.is_running()) {
            // Wait for an explicit dirty notification or the periodic 200 ms
            // audio-level refresh window. Checking dirty *before* computing
            // state prevents a markDirty() that fires while we are broadcasting
            // from being lost.
            {
                std::unique_lock<std::mutex> lk(dirtyMutex);
                dirtyCv.wait_for(lk, std::chrono::milliseconds(200),
                                  [this] { return dirty || !svr.is_running(); });
                dirty = false;
            }

            std::string current = stateJson(matrix).dump();
            if (current != lastBroadcastState) {
                lastBroadcastState = current;
                pushToAllClients("data: " + current + "\n\n");
            }
        }
    }

    void registerRoutes();
};

MixerControlServer::MixerControlServer(AudioMixerMatrix& matrix)
    : m_impl(std::make_unique<Impl>(matrix)) {}

MixerControlServer::~MixerControlServer() { stop(); }

bool MixerControlServer::start(uint16_t port)
{
    m_impl->registerRoutes();

    m_impl->svr.new_task_queue = [] { return new httplib::ThreadPool(8); };

    if (!m_impl->svr.bind_to_port("127.0.0.1", port)) {
        std::fprintf(stderr, "[mixer-api] Could not bind 127.0.0.1:%u\n", port);
        return false;
    }

    m_port = port;
    m_running = true;
    m_impl->listenThread = std::thread([this] { m_impl->svr.listen_after_bind(); });
    m_impl->broadcastThread = std::thread([this] { m_impl->broadcastLoop(); });
    return true;
}

void MixerControlServer::stop()
{
    m_impl->svr.stop();
    m_impl->dirtyCv.notify_all();
    if (m_impl->listenThread.joinable()) m_impl->listenThread.join();
    if (m_impl->broadcastThread.joinable()) m_impl->broadcastThread.join();
    m_running = false;
}

void MixerControlServer::setAutosavePath(const std::string& path)
{
    m_impl->setAutosavePath(path);
}

void MixerControlServer::Impl::registerRoutes()
{
    svr.set_default_headers({
        { "Access-Control-Allow-Origin", "*" },
        { "Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS" },
        { "Access-Control-Allow-Headers", "Content-Type" },
    });
    svr.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    svr.Get("/api/state", [this](const httplib::Request&, httplib::Response& res) {
        sendJson(res, stateJson(matrix));
    });

    svr.Get("/api/endpoints", [this](const httplib::Request&, httplib::Response& res) {
        EnsureComInitializedOnThisThread();
        auto eps = matrix.listEndpoints();
        auto arr = nlohmann::json::array();
        for (auto& e : eps) arr.push_back(toJson(e));
        sendJson(res, nlohmann::json{ { "endpoints", arr } });
    });

    svr.Get("/api/applications", [this](const httplib::Request&, httplib::Response& res) {
        EnsureComInitializedOnThisThread();
        auto apps = matrix.listApplications();
        auto arr = nlohmann::json::array();
        for (const auto& a : apps) arr.push_back(toJson(a));
        sendJson(res, nlohmann::json{ { "applications", arr } });
    });

    svr.Get("/api/events", [this](const httplib::Request& req, httplib::Response& res) {
        uint64_t clientId = nextClientId.fetch_add(1);
        auto client = std::make_shared<SseClient>();
        {
            std::lock_guard<std::mutex> lk(clientsMutex);
            clients[clientId] = client;
        }

        res.set_chunked_content_provider("text/event-stream",
            [this, client, clientId](size_t, httplib::DataSink& sink) {
                std::unique_lock<std::mutex> lk(client->mutex);
                bool got = client->cv.wait_for(lk, std::chrono::seconds(5),
                    [&] { return !client->queue.empty() || client->closed; });
                if (client->closed) return false;
                if (got && !client->queue.empty()) {
                    auto chunk = std::move(client->queue.front());
                    client->queue.pop_front();
                    lk.unlock();
                    return sink.write(chunk.data(), chunk.size());
                }
                // heartbeat: keep idle SSE connections alive
                lk.unlock();
                std::string hb = ": hb\n\n";
                return sink.write(hb.data(), hb.size());
            },
            [this, client, clientId](bool) {
                std::lock_guard<std::mutex> lk(client->mutex);
                client->closed = true;
                std::lock_guard<std::mutex> clk(clientsMutex);
                clients.erase(clientId);
            }
        );

        // Send initial state immediately.
        std::lock_guard<std::mutex> clk(client->mutex);
        client->queue.push_back("data: " + stateJson(matrix).dump() + "\n\n");
        client->cv.notify_all();
    });

    // -----------------------------------------------------------------------
    // Inputs
    // -----------------------------------------------------------------------
    svr.Post("/api/inputs", [this](const httplib::Request& req, httplib::Response& res) {
        EnsureComInitializedOnThisThread();
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (const std::exception&) { sendError(res, 400, "invalid JSON body"); return; }

        InputConfig cfg;
        cfg.name     = body.value("name", std::string{});
        cfg.type     = body.value("type", std::string{"device"});
        cfg.source   = body.value("source", std::string{});
        cfg.denoise  = body.value("denoise", false);
        cfg.eqPreset = body.value("eqPreset", std::string{});
        cfg.spatial  = body.value("spatial", false);
        cfg.azimuth  = body.value("azimuth", 0.0f);
        cfg.elevation= body.value("elevation", 0.0f);
        if (cfg.name.empty() || cfg.source.empty()) {
            sendError(res, 400, "missing required fields 'name' and 'source'"); return;
        }

        auto id = matrix.addInput(cfg);
        if (!id) { sendError(res, 400, "could not add input"); return; }

        markDirty();
        auto snap = matrix.snapshot();
        for (const auto& in : snap.inputs) {
            if (in.id == *id) { sendJson(res, toJson(in), 201); return; }
        }
        sendJson(res, nlohmann::json{{"id", *id}}, 201);
    });

    svr.Patch(R"(/api/inputs/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        EnsureComInitializedOnThisThread();
        InputId id = static_cast<InputId>(std::stoull(req.matches[1]));

        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (const std::exception&) { sendError(res, 400, "invalid JSON body"); return; }

        // Merge with the current input so a partial PATCH (e.g. only
        // {"denoise": true}) leaves every other field untouched.
        auto preSnap = matrix.snapshot();
        const InputSnapshot* cur = nullptr;
        for (const auto& in : preSnap.inputs) {
            if (in.id == id) { cur = &in; break; }
        }
        if (cur == nullptr) { sendError(res, 404, "input not found"); return; }

        InputConfig cfg;
        cfg.name     = body.value("name", cur->name);
        cfg.type     = body.value("type", cur->type);
        cfg.source   = body.value("source", cur->source);
        cfg.denoise  = body.value("denoise", cur->denoise);
        cfg.eqPreset = body.value("eqPreset", cur->eqPreset);
        cfg.spatial  = body.value("spatial", cur->spatial);
        cfg.azimuth  = body.value("azimuth", cur->azimuth);
        cfg.elevation= body.value("elevation", cur->elevation);
        if (cfg.name.empty() || cfg.source.empty()) {
            sendError(res, 400, "'name' and 'source' cannot be empty"); return;
        }

        if (!matrix.updateInput(id, cfg)) { sendError(res, 404, "input not found"); return; }
        markDirty();

        auto snap = matrix.snapshot();
        for (const auto& in : snap.inputs) {
            if (in.id == id) { sendJson(res, toJson(in)); return; }
        }
        sendJson(res, nlohmann::json{{"id", id}}, 202);
    });

    // Live HRTF direction change. Unlike PATCH (which rebuilds the input's
    // strips), this updates the running spatializer in place — glitch-free and
    // safe to call at knob-turn rates. Body: {"azimuth": deg, "elevation": deg}.
    svr.Post(R"(/api/inputs/(\d+)/direction)", [this](const httplib::Request& req, httplib::Response& res) {
        InputId id = static_cast<InputId>(std::stoull(req.matches[1]));
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (const std::exception&) { sendError(res, 400, "invalid JSON body"); return; }

        auto snap = matrix.snapshot();
        const InputSnapshot* cur = nullptr;
        for (const auto& in : snap.inputs) if (in.id == id) { cur = &in; break; }
        if (cur == nullptr) { sendError(res, 404, "input not found"); return; }

        float az = body.value("azimuth", cur->azimuth);
        float el = body.value("elevation", cur->elevation);
        if (!matrix.setInputDirection(id, az, el)) { sendError(res, 404, "input not found"); return; }
        markDirty();

        auto after = matrix.snapshot();
        for (const auto& in : after.inputs) if (in.id == id) { sendJson(res, toJson(in)); return; }
        sendJson(res, nlohmann::json{{"id", id}, {"azimuth", az}, {"elevation", el}});
    });

    svr.Delete(R"(/api/inputs/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        InputId id = static_cast<InputId>(std::stoull(req.matches[1]));
        if (!matrix.removeInput(id)) { sendError(res, 404, "input not found"); return; }
        markDirty();
        res.status = 204;
    });

    // -----------------------------------------------------------------------
    // Groups
    // -----------------------------------------------------------------------
    svr.Post("/api/groups", [this](const httplib::Request& req, httplib::Response& res) {
        EnsureComInitializedOnThisThread();
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (const std::exception&) { sendError(res, 400, "invalid JSON body"); return; }

        GroupConfig cfg;
        cfg.name   = body.value("name", std::string{});
        cfg.color  = body.value("color", std::string{"#3b82f6"});
        cfg.cable  = body.value("cable", std::string{});
        cfg.volume = body.value("volume", 100.0f) / 100.0f;
        cfg.muted  = body.value("muted", false);
        if (body.contains("inputIds")) cfg.inputIds = body["inputIds"].get<std::vector<InputId>>();
        if (body.contains("outputIds")) cfg.outputIds = body["outputIds"].get<std::vector<std::string>>();
        cfg.knobIndex = parseOptionalInt(body, "knobIndex");

        if (cfg.name.empty()) { sendError(res, 400, "missing required field 'name'"); return; }

        auto id = matrix.addGroup(cfg);
        if (!id) { sendError(res, 400, "could not create group"); return; }

        markDirty();
        auto snap = matrix.snapshot();
        for (const auto& g : snap.groups) {
            if (g.id == *id) { sendJson(res, toJson(g), 201); return; }
        }
        sendJson(res, nlohmann::json{{"id", *id}}, 201);
    });

    svr.Patch(R"(/api/groups/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        EnsureComInitializedOnThisThread();
        GroupId id = static_cast<GroupId>(std::stoull(req.matches[1]));

        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (const std::exception&) { sendError(res, 400, "invalid JSON body"); return; }

        auto snap0 = matrix.snapshot();
        const GroupSnapshot* existing = nullptr;
        for (const auto& g : snap0.groups) if (g.id == id) { existing = &g; break; }
        if (!existing) { sendError(res, 404, "group not found"); return; }

        bool changed = false;
        if (body.contains("name") && body["name"].is_string()) {
            if (matrix.setGroupName(id, body["name"].get<std::string>())) changed = true;
        }
        if (body.contains("color") && body["color"].is_string()) {
            if (matrix.setGroupColor(id, body["color"].get<std::string>())) changed = true;
        }
        if (body.contains("cable") && body["cable"].is_string()) {
            if (matrix.setGroupCable(id, body["cable"].get<std::string>())) changed = true;
        }
        if (body.contains("volume")) {
            if (matrix.setGroupVolume(id, body["volume"].get<float>())) changed = true;
        }
        if (body.contains("muted")) {
            if (matrix.setGroupMuted(id, body["muted"].get<bool>())) changed = true;
        }
        if (body.contains("inputIds")) {
            auto ids = body["inputIds"].get<std::vector<InputId>>();
            if (matrix.setGroupInputIds(id, ids)) changed = true;
        }
        if (body.contains("outputIds")) {
            auto ids = body["outputIds"].get<std::vector<std::string>>();
            if (matrix.setGroupOutputIds(id, ids)) changed = true;
        }
        if (body.contains("outputGains") && body["outputGains"].is_object()) {
            for (const auto& kv : body["outputGains"].items()) {
                if (matrix.setGroupOutputGain(id, kv.key(), kv.value().get<float>())) changed = true;
            }
        }
        if (body.contains("knobIndex")) {
            std::optional<int> k;
            if (!body["knobIndex"].is_null()) k = body["knobIndex"].get<int>();
            if (matrix.setGroupKnobIndex(id, k)) changed = true;
        }

        if (!changed) { sendError(res, 400, "no valid fields to update"); return; }
        markDirty();

        auto snap = matrix.snapshot();
        for (const auto& g : snap.groups) {
            if (g.id == id) { sendJson(res, toJson(g)); return; }
        }
        sendJson(res, toJson(*existing)); // fallback
    });

    svr.Delete(R"(/api/groups/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        GroupId id = static_cast<GroupId>(std::stoull(req.matches[1]));
        if (!matrix.removeGroup(id)) { sendError(res, 404, "group not found"); return; }
        markDirty();
        res.status = 204;
    });

    // -----------------------------------------------------------------------
    // Outputs
    // -----------------------------------------------------------------------
    svr.Post("/api/outputs", [this](const httplib::Request& req, httplib::Response& res) {
        EnsureComInitializedOnThisThread();
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (const std::exception&) { sendError(res, 400, "invalid JSON body"); return; }

        if (!body.contains("name") || !body["name"].is_string() ||
            body["name"].get<std::string>().empty()) {
            sendError(res, 400, "missing required field 'name'"); return;
        }

        std::string name = body["name"].get<std::string>();
        for (const auto& existing : matrix.outputNames()) {
            if (existing == name) { sendError(res, 409, "output already exists"); return; }
        }

        if (!matrix.addOutput(name)) {
            sendError(res, 400, "could not open output endpoint"); return;
        }

        markDirty();
        nlohmann::json j;
        j["name"]   = name;
        j["master"] = matrix.outputMasterVolume(name);
        sendJson(res, j, 201);
    });

    svr.Delete("/api/outputs", [this](const httplib::Request& req, httplib::Response& res) {
        EnsureComInitializedOnThisThread();
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (const std::exception&) { sendError(res, 400, "invalid JSON body"); return; }
        if (!body.contains("name") || !body["name"].is_string()) {
            sendError(res, 400, "missing required field 'name'"); return;
        }
        if (!matrix.removeOutput(body["name"].get<std::string>())) {
            sendError(res, 404, "output not found"); return;
        }
        markDirty();
        res.status = 204;
    });

    svr.Post("/api/outputs/master", [this](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (const std::exception&) { sendError(res, 400, "invalid JSON body"); return; }
        if (!body.contains("name") || (!body.contains("volume") && !body.contains("muted"))) {
            sendError(res, 400, "missing required field 'name' plus 'volume' and/or 'muted'"); return;
        }
        std::string name = body["name"].get<std::string>();
        if (std::find(matrix.outputNames().begin(), matrix.outputNames().end(), name) == matrix.outputNames().end()) {
            sendError(res, 404, "output not found"); return;
        }
        if (body.contains("volume")) matrix.setOutputMasterVolume(name, body["volume"].get<float>());
        if (body.contains("muted"))  matrix.setOutputMuted(name, body["muted"].get<bool>());
        markDirty();
        nlohmann::json j;
        j["name"]   = name;
        j["master"] = matrix.outputMasterVolume(name);
        sendJson(res, j);
    });

    // -----------------------------------------------------------------------
    // Presets
    // -----------------------------------------------------------------------
    svr.Get("/api/scenes", [this](const httplib::Request&, httplib::Response& res) {
        namespace fs = std::filesystem;
        auto arr = nlohmann::json::array();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(this->sceneDir(), ec)) {
            if (e.path().extension() == ".json") {
                arr.push_back(nlohmann::json{ { "name", e.path().stem().string() } });
            }
        }
        sendJson(res, nlohmann::json{ { "scenes", arr } });
    });

    svr.Post("/api/scenes/save", [this](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (const std::exception&) { sendError(res, 400, "invalid JSON body"); return; }
        std::string name = body.value("name", std::string{});
        if (!isSafeSceneName(name)) {
            sendError(res, 400, "scene name must be 1-64 chars of letters, digits, space, - or _"); return;
        }

        auto snap = matrix.snapshot();
        nlohmann::json scene;
        scene["name"] = name;
        scene["groups"] = nlohmann::json::array();
        for (const auto& g : snap.groups) {
            scene["groups"].push_back(nlohmann::json{
                { "name", g.name }, { "volume", g.volume }, { "muted", g.muted },
                { "outputGains", g.outputGains } });
        }
        scene["outputs"] = nlohmann::json::array();
        for (const auto& o : snap.outputs) {
            scene["outputs"].push_back(nlohmann::json{
                { "name", o.name }, { "master", o.master }, { "muted", o.muted } });
        }

        std::error_code ec;
        std::filesystem::create_directories(this->sceneDir(), ec);
        std::ofstream f(this->scenePath(name));
        if (!f) { sendError(res, 500, "could not write scene file"); return; }
        f << scene.dump(2);
        sendJson(res, nlohmann::json{ { "name", name } }, 201);
    });

    svr.Post("/api/scenes/apply", [this](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (const std::exception&) { sendError(res, 400, "invalid JSON body"); return; }
        std::string name = body.value("name", std::string{});
        if (!isSafeSceneName(name)) { sendError(res, 400, "invalid scene name"); return; }

        std::ifstream f(this->scenePath(name));
        if (!f) { sendError(res, 404, "scene not found"); return; }
        nlohmann::json scene;
        try { f >> scene; }
        catch (const std::exception&) { sendError(res, 500, "scene file is not valid JSON"); return; }

        // Apply by name so scenes survive group ids changing between sessions.
        auto snap = matrix.snapshot();
        std::unordered_map<std::string, GroupId> groupIds;
        for (const auto& g : snap.groups) groupIds[g.name] = g.id;

        int applied = 0;
        auto missing = nlohmann::json::array();
        if (scene.contains("groups") && scene["groups"].is_array()) {
            for (const auto& g : scene["groups"]) {
                std::string gname = g.value("name", std::string{});
                auto it = groupIds.find(gname);
                if (it == groupIds.end()) { missing.push_back(gname); continue; }
                if (g.contains("volume")) matrix.setGroupVolume(it->second, g["volume"].get<float>());
                if (g.contains("muted"))  matrix.setGroupMuted(it->second, g["muted"].get<bool>());
                if (g.contains("outputGains") && g["outputGains"].is_object()) {
                    for (const auto& kv : g["outputGains"].items()) {
                        matrix.setGroupOutputGain(it->second, kv.key(), kv.value().get<float>());
                    }
                }
                ++applied;
            }
        }
        if (scene.contains("outputs") && scene["outputs"].is_array()) {
            for (const auto& o : scene["outputs"]) {
                std::string oname = o.value("name", std::string{});
                if (oname.empty()) continue;
                const auto names = matrix.outputNames();
                if (std::find(names.begin(), names.end(), oname) == names.end()) {
                    missing.push_back(oname); continue;
                }
                if (o.contains("master")) matrix.setOutputMasterVolume(oname, o["master"].get<float>());
                if (o.contains("muted"))  matrix.setOutputMuted(oname, o["muted"].get<bool>());
                ++applied;
            }
        }

        markDirty();
        sendJson(res, nlohmann::json{ { "name", name }, { "applied", applied }, { "missing", missing } });
    });

    svr.Post("/api/presets/save", [this](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (const std::exception&) { sendError(res, 400, "invalid JSON body"); return; }

        std::string path = body.value("path", std::string{});
        if (path.empty()) path = autosavePath;
        if (path.empty()) { sendError(res, 400, "missing 'path' and no autosave path configured"); return; }

        if (!isSafePresetPath(path)) {
            sendError(res, 400, "path must be a relative path with no '..' segments"); return;
        }

        if (!matrix.save(path)) {
            sendError(res, 500, "failed to save preset"); return;
        }

        // If a path was supplied, also make it the autosave target.
        if (!autosavePath.empty() || body.contains("path")) {
            setAutosavePath(path);
        }
        sendJson(res, nlohmann::json{ { "saved", path } });
    });
}

} // namespace anniaudio::core
