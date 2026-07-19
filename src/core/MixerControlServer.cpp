#include "MixerControlServer.hpp"
#include "AudioMixerMatrix.hpp"

#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <windows.h>

// httplib pulls in winsock2.h and links ws2_32 itself (via #pragma comment
// under MSVC) -- nothing extra needed in CMakeLists beyond the include path.
#include <httplib/httplib.h>

#include <nlohmann/json.hpp>

namespace anniaudio::core {

namespace {

// httplib's thread-pool worker threads are long-lived (reused across many
// requests) and never explicitly torn down by us, so CoInitializeEx is
// called at most once per worker thread and deliberately never paired with
// CoUninitialize -- the thread pool outlives the server, and the process
// exiting cleans up COM regardless.
void EnsureComInitializedOnThisThread()
{
    thread_local bool initialized = false;
    if (initialized) return;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    initialized = true;
}

nlohmann::json toJson(const StripSnapshot& s)
{
    nlohmann::json j;
    j["id"]     = s.id;
    j["name"]   = s.name;
    j["source"] = s.source;
    j["output"] = s.output;
    j["volume"] = s.volume * 100.0f; // API speaks in 0-200 percent
    j["muted"]  = s.muted;
    j["peak"]   = s.peak;
    j["rms"]    = s.rms;
    j["knobIndex"] = s.knobIndex.has_value() ? nlohmann::json(*s.knobIndex) : nlohmann::json(nullptr);
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

nlohmann::json stateJson(AudioMixerMatrix& matrix)
{
    nlohmann::json j;
    j["running"] = matrix.running();

    auto outputs = nlohmann::json::array();
    for (const auto& name : matrix.outputs()) {
        nlohmann::json out;
        out["name"]       = name;
        out["master"]     = matrix.outputMasterVolume(name) * 100.0f;
        out["masterPeak"] = matrix.outputMasterPeak(name);
        out["masterRms"]  = matrix.outputMasterRms(name);
        out["strips"]     = nlohmann::json::array();

        for (auto& s : matrix.snapshot()) {
            if (s.output == name) out["strips"].push_back(toJson(s));
        }
        outputs.push_back(std::move(out));
    }
    j["outputs"] = std::move(outputs);
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

// Only used for POST /api/presets/save -- keep writes confined to the repo's
// config tree rather than letting a client (even a local one) write anywhere
// on disk.
bool isSafePresetPath(const std::string& path)
{
    if (path.empty()) return false;
    if (path.find("..") != std::string::npos) return false;
    if (path.size() > 1 && (path[0] == '/' || path[0] == '\\')) return false;
    if (path.size() > 2 && path[1] == ':') return false; // e.g. "C:\..."
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct MixerControlServer::Impl {
    AudioMixerMatrix& matrix;

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
        while (svr.is_running()) {
            std::string current = stateJson(matrix).dump();
            bool changed = current != lastBroadcastState;
            if (changed) {
                lastBroadcastState = current;
                pushToAllClients("data: " + current + "\n\n");
            }

            std::unique_lock<std::mutex> lk(dirtyMutex);
            dirty = false;
            dirtyCv.wait_for(lk, std::chrono::milliseconds(200),
                              [this] { return dirty || !svr.is_running(); });
        }
    }

    void registerRoutes();
};

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

    svr.Post("/api/outputs", [this](const httplib::Request& req, httplib::Response& res) {
        EnsureComInitializedOnThisThread();
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (const std::exception&) { sendError(res, 400, "invalid JSON body"); return; }

        if (!body.contains("name") || !body["name"].is_string() ||
            body["name"].get<std::string>().empty()) {
            sendError(res, 400, "missing required field 'name'");
            return;
        }

        std::string name = body["name"].get<std::string>();
        for (const auto& existing : matrix.outputs()) {
            if (existing == name) {
                sendError(res, 409, "output already exists");
                return;
            }
        }

        if (!matrix.addOutput(name)) {
            sendError(res, 400, "could not open output endpoint");
            return;
        }

        markDirty();
        nlohmann::json j;
        j["name"]   = name;
        j["master"] = matrix.outputMasterVolume(name) * 100.0f;
        sendJson(res, j, 201);
    });

    svr.Delete("/api/outputs", [this](const httplib::Request& req, httplib::Response& res) {
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
        if (!body.contains("name") || !body.contains("volume")) {
            sendError(res, 400, "missing required fields 'name' and 'volume'"); return;
        }
        std::string name = body["name"].get<std::string>();
        if (std::find(matrix.outputs().begin(), matrix.outputs().end(), name) == matrix.outputs().end()) {
            sendError(res, 404, "output not found"); return;
        }
        matrix.setOutputMasterVolume(name, body["volume"].get<float>() / 100.0f);
        markDirty();
        sendJson(res, nlohmann::json{ { "name", name }, { "master", matrix.outputMasterVolume(name) * 100.0f } });
    });

    svr.Post("/api/strips", [this](const httplib::Request& req, httplib::Response& res) {
        EnsureComInitializedOnThisThread();
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (const std::exception&) { sendError(res, 400, "invalid JSON body"); return; }

        if (!body.contains("source") || !body["source"].is_string() || body["source"].get<std::string>().empty() ||
            !body.contains("output") || !body["output"].is_string() || body["output"].get<std::string>().empty()) {
            sendError(res, 400, "missing required fields 'source' and 'output'");
            return;
        }

        std::string source = body["source"].get<std::string>();
        std::string output = body["output"].get<std::string>();

        auto existing = matrix.outputs();
        if (std::find(existing.begin(), existing.end(), output) == existing.end()) {
            sendError(res, 404, "output not found"); return;
        }

        MixerStripConfig cfg;
        cfg.source = source;
        cfg.name   = body.value("name", std::string{});
        cfg.volume = body.value("volume", 100.0f) / 100.0f;
        cfg.muted  = body.value("muted", false);
        if (body.contains("knobIndex") && !body["knobIndex"].is_null()) {
            cfg.knobIndex = body["knobIndex"].get<int>();
        }

        auto id = matrix.addRoute(source, output, cfg);
        if (!id) {
            sendError(res, 400, "could not open source (not found, or strip limit reached)");
            return;
        }
        markDirty();
        auto snap = matrix.routeSnapshot(*id);
        sendJson(res, toJson(*snap), 201);
    });

    svr.Patch(R"(/api/strips/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        AudioMixerMatrix::RouteId id = std::stoull(req.matches[1]);
        if (!matrix.routeSnapshot(id)) { sendError(res, 404, "unknown strip id"); return; }

        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (const std::exception&) { sendError(res, 400, "invalid JSON body"); return; }

        if (body.contains("name") && body["name"].is_string()) {
            matrix.renameRoute(id, body["name"].get<std::string>());
        }
        if (body.contains("volume")) {
            matrix.setRouteVolume(id, body["volume"].get<float>() / 100.0f);
        }
        if (body.contains("muted")) {
            matrix.setRouteMuted(id, body["muted"].get<bool>());
        }
        if (body.contains("knobIndex")) {
            if (body["knobIndex"].is_null()) matrix.setRouteKnobIndex(id, std::nullopt);
            else matrix.setRouteKnobIndex(id, body["knobIndex"].get<int>());
        }

        markDirty();
        auto snap = matrix.routeSnapshot(id);
        sendJson(res, toJson(*snap));
    });

    svr.Delete(R"(/api/strips/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        AudioMixerMatrix::RouteId id = std::stoull(req.matches[1]);
        if (!matrix.routeSnapshot(id)) { sendError(res, 404, "unknown strip id"); return; }
        matrix.removeRoute(id);
        markDirty();
        res.status = 204;
    });

    svr.Post("/api/presets/save", [this](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (const std::exception&) { sendError(res, 400, "invalid JSON body"); return; }
        if (!body.contains("path") || !body["path"].is_string()) {
            sendError(res, 400, "missing required field 'path'"); return;
        }
        std::string path = body["path"].get<std::string>();
        if (!isSafePresetPath(path)) {
            sendError(res, 400, "path must be a relative path with no '..' segments"); return;
        }

        nlohmann::json out;
        out["outputs"] = nlohmann::json::array();
        out["routes"]  = nlohmann::json::array();

        for (const auto& name : matrix.outputs()) {
            nlohmann::json oj;
            oj["name"]   = name;
            oj["master"] = matrix.outputMasterVolume(name) * 100.0f;
            out["outputs"].push_back(oj);
        }

        for (auto& s : matrix.snapshot()) {
            nlohmann::json sj;
            sj["source"] = s.source;
            sj["output"] = s.output;
            sj["name"]   = s.name;
            sj["volume"] = s.volume * 100.0f;
            sj["muted"]  = s.muted;
            if (s.knobIndex) sj["knobIndex"] = *s.knobIndex;
            out["routes"].push_back(sj);
        }

        std::ofstream f(path);
        if (!f) { sendError(res, 500, "failed to open file for writing"); return; }
        f << out.dump(2) << "\n";
        sendJson(res, nlohmann::json{ { "saved", path } });
    });

    svr.Get("/api/events", [this](const httplib::Request&, httplib::Response& res) {
        auto client = std::make_shared<SseClient>();
        uint64_t id = nextClientId.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(clientsMutex);
            clients[id] = client;
        }
        {
            std::lock_guard<std::mutex> lk(client->mutex);
            client->queue.push_back("data: " + stateJson(matrix).dump() + "\n\n");
        }

        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_chunked_content_provider(
            "text/event-stream",
            [this, client](size_t /*offset*/, httplib::DataSink& sink) {
                std::string chunk;
                {
                    std::unique_lock<std::mutex> lk(client->mutex);
                    if (client->queue.empty() && !client->closed) {
                        client->cv.wait_for(lk, std::chrono::seconds(15),
                            [&] { return !client->queue.empty() || client->closed; });
                    }
                    if (client->closed) return false;
                    if (client->queue.empty()) {
                        chunk = ": heartbeat\n\n";
                    } else {
                        chunk = std::move(client->queue.front());
                        client->queue.pop_front();
                    }
                }
                return sink.write(chunk.data(), chunk.size());
            },
            [this, id](bool /*success*/) {
                std::lock_guard<std::mutex> lk(clientsMutex);
                clients.erase(id);
            });
    });
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

MixerControlServer::MixerControlServer(AudioMixerMatrix& matrix)
    : m_impl(std::make_unique<Impl>(matrix))
{
}

MixerControlServer::~MixerControlServer() { stop(); }

bool MixerControlServer::start(uint16_t port)
{
    if (m_running.load()) return false;
    m_port = port;

    m_impl->registerRoutes();
    // Bind to the IPv4 loopback only; AF_UNSPEC can resolve ::1 first on some
    // Windows configurations and end up on a different address than clients
    // expecting 127.0.0.1.
    m_impl->svr.set_address_family(AF_INET);
    m_impl->listenThread = std::thread([this] {
        if (!m_impl->svr.listen("127.0.0.1", m_port)) {
            std::fprintf(stderr, "[mixer-api] Failed to bind 127.0.0.1:%u\n", m_port);
        }
    });

    for (int i = 0; i < 50 && !m_impl->svr.is_running(); ++i) Sleep(10);
    if (!m_impl->svr.is_running()) {
        if (m_impl->listenThread.joinable()) m_impl->listenThread.join();
        return false;
    }

    m_impl->broadcastThread = std::thread([this] { m_impl->broadcastLoop(); });
    m_running = true;
    std::fprintf(stderr, "[mixer-api] Listening on http://127.0.0.1:%u\n", m_port);
    return true;
}

void MixerControlServer::stop()
{
    if (!m_running.exchange(false)) return;

    m_impl->svr.stop();
    {
        std::lock_guard<std::mutex> lk(m_impl->dirtyMutex);
        m_impl->dirty = true;
    }
    m_impl->dirtyCv.notify_all();

    {
        std::lock_guard<std::mutex> lk(m_impl->clientsMutex);
        for (auto& [id, client] : m_impl->clients) {
            std::lock_guard<std::mutex> clk(client->mutex);
            client->closed = true;
            client->cv.notify_all();
        }
    }

    if (m_impl->listenThread.joinable()) m_impl->listenThread.join();
    if (m_impl->broadcastThread.joinable()) m_impl->broadcastThread.join();
}

} // namespace anniaudio::core
