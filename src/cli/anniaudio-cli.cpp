// anniaudio-cli — standalone command-line client for the AnniAudio mixer API.
//
// Usage:
//   anniaudio-cli [--port <port>] [--api-key <key>] <command> [args]
//
// Commands:
//   state
//   inputs | groups | outputs | endpoints | applications | scenes
//   set-volume <group> <0-200>
//   set-master <output> <0-200>
//   mute <group> [on|off|toggle]
//   output-mute <output> [on|off|toggle]
//   set-direction <input> <azimuth> [elevation]
//   set-hrtf <input> <sofa-path>
//   apply-scene <name>
//   save-scene <name>
//   save-preset <path>
//   load-preset <path>

#include <httplib/httplib.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace {

struct Ctx {
    httplib::Client client;
    nlohmann::json state;
    bool haveState = false;

    Ctx(const std::string& host, int port, const std::string& apiKey)
        : client(host + ":" + std::to_string(port))
    {
        if (!apiKey.empty()) {
            client.set_default_headers({{"X-API-Key", apiKey}});
        }
    }

    bool fetchState()
    {
        auto res = client.Get("/api/state");
        if (!res || res->status != 200) return false;
        try {
            state = nlohmann::json::parse(res->body);
            haveState = true;
            return true;
        } catch (...) { return false; }
    }

    nlohmann::json get(const std::string& path)
    {
        auto res = client.Get(path.c_str());
        if (!res || res->status != 200) return {};
        try { return nlohmann::json::parse(res->body); } catch (...) { return {}; }
    }

    bool patch(const std::string& path, const nlohmann::json& body)
    {
        auto j = body.dump();
        auto res = client.Patch(path.c_str(), j, "application/json");
        return res && (res->status == 200 || res->status == 204);
    }

    bool post(const std::string& path, const nlohmann::json& body)
    {
        auto j = body.dump();
        auto res = client.Post(path.c_str(), j, "application/json");
        return res && (res->status == 200 || res->status == 204);
    }

    std::optional<nlohmann::json> findGroup(const std::string& name)
    {
        if (!haveState && !fetchState()) return std::nullopt;
        for (const auto& g : state["groups"]) {
            if (g.value("name", std::string{}) == name) return g;
        }
        return std::nullopt;
    }

    std::optional<nlohmann::json> findInput(const std::string& name)
    {
        if (!haveState && !fetchState()) return std::nullopt;
        for (const auto& i : state["inputs"]) {
            if (i.value("name", std::string{}) == name) return i;
        }
        return std::nullopt;
    }
};

std::vector<std::string> toVector(int argc, char* argv[])
{
    std::vector<std::string> v;
    v.reserve(argc);
    for (int i = 0; i < argc; ++i) v.emplace_back(argv[i]);
    return v;
}

int toInt(const std::string& s)
{
    return std::atoi(s.c_str());
}

bool parseBool(const std::string& s)
{
    std::string t;
    for (auto c : s) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t == "on" || t == "true" || t == "1" || t == "yes";
}

void printUsage(const char* prog)
{
    std::printf(R"(
Usage: %s [--port <port>] [--api-key <key>] <command> [args]

  --api-key is required when the mixer is configured with an API key.

Commands:
  state                          print mixer state summary
  inputs | groups | outputs      list mixer elements
  endpoints | applications       list audio endpoints / sessions
  scenes                         list saved scenes

  set-volume <group> <0-200>
  set-master <output> <0-200>
  mute <group> [on|off|toggle]     default: toggle
  output-mute <output> [on|off|toggle]

  set-direction <input> <azimuth> [elevation]  (for spatial inputs)
  set-hrtf <input> <sofa-path>                  (for spatial inputs)
  apply-scene <name>
  save-scene <name>
  save-preset <path>
  load-preset <path>

Examples:
  %s --port 8850 set-volume Music 80
  %s mute Music
  %s set-direction Microphone 30 -5
  %s set-hrtf Microphone assets/hrtf/sadie.sofa
  %s --api-key secret --port 8850 state

)", prog, prog, prog, prog, prog, prog);
}

} // namespace

int main(int argc, char* argv[])
{
    auto args = toVector(argc, argv);
    if (args.size() < 2) { printUsage(args[0].c_str()); return 1; }

    int port = 8850;
    std::string apiKey;
    std::vector<std::string> positional;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--port" && i + 1 < args.size()) {
            port = std::atoi(args[++i].c_str());
        } else if (args[i] == "--api-key" && i + 1 < args.size()) {
            apiKey = args[++i];
        } else if (args[i] == "--help" || args[i] == "-h") {
            printUsage(args[0].c_str()); return 0;
        } else {
            positional.push_back(args[i]);
        }
    }

    if (positional.empty()) { printUsage(args[0].c_str()); return 1; }

    Ctx ctx("http://127.0.0.1", port, apiKey);
    ctx.client.set_connection_timeout(2, 0);

    const std::string& cmd = positional[0];

    if (cmd == "state") {
        auto res = ctx.client.Get("/api/state");
        if (!res || res->status != 200) { std::fprintf(stderr, "API not reachable (port %d)\n", port); return 1; }
        auto j = nlohmann::json::parse(res->body);
        std::printf("running: %s\n", j.value("running", false) ? "true" : "false");
        std::printf("controlPort: %d\n", j.value("controlPort", 0));
        std::printf("inputs (%zu):\n", j["inputs"].size());
        for (const auto& i : j["inputs"]) {
            std::printf("  [%u] %s (%s) peak=%.3f rms=%.3f\n",
                i.value("id", 0u), i.value("name", "?").c_str(),
                i.value("source", "?").c_str(),
                i.value("peak", 0.0), i.value("rms", 0.0));
        }
        std::printf("groups (%zu):\n", j["groups"].size());
        for (const auto& g : j["groups"]) {
            std::printf("  [%u] %s vol=%.0f%% muted=%s\n",
                g.value("id", 0u), g.value("name", "?").c_str(),
                g.value("volume", 0.0),
                g.value("muted", false) ? "yes" : "no");
        }
        std::printf("outputs (%zu):\n", j["outputs"].size());
        for (const auto& o : j["outputs"]) {
            std::printf("  %s master=%.0f%% muted=%s\n",
                o.value("name", "?").c_str(),
                o.value("master", 0.0),
                o.value("muted", false) ? "yes" : "no");
        }
        return 0;
    }

    if (cmd == "inputs" || cmd == "groups" || cmd == "outputs") {
        auto j = ctx.get("/api/state");
        if (j.empty()) { std::fprintf(stderr, "API not reachable (port %d)\n", port); return 1; }
        auto key = cmd;
        for (const auto& item : j[key]) {
            std::cout << item.dump(2) << "\n";
        }
        return 0;
    }

    if (cmd == "endpoints") {
        auto j = ctx.get("/api/endpoints");
        for (const auto& e : j.value("endpoints", nlohmann::json::array())) std::cout << e.dump(2) << "\n";
        return 0;
    }

    if (cmd == "applications") {
        auto j = ctx.get("/api/applications");
        for (const auto& a : j.value("applications", nlohmann::json::array())) std::cout << a.dump(2) << "\n";
        return 0;
    }

    if (cmd == "scenes") {
        auto j = ctx.get("/api/scenes");
        for (const auto& s : j.value("scenes", nlohmann::json::array())) std::cout << s.dump(2) << "\n";
        return 0;
    }

    if (cmd == "set-volume") {
        if (positional.size() < 3) { std::fprintf(stderr, "usage: set-volume <group> <0-200>\n"); return 1; }
        auto g = ctx.findGroup(positional[1]);
        if (!g) { std::fprintf(stderr, "group not found: %s\n", positional[1].c_str()); return 1; }
        int vol = toInt(positional[2]);
        if (!ctx.patch("/api/groups/" + std::to_string(g->value("id", 0u)), {{"volume", vol}})) { std::fprintf(stderr, "failed\n"); return 1; }
        std::printf("group '%s' volume -> %d%%\n", positional[1].c_str(), vol);
        return 0;
    }

    if (cmd == "set-master") {
        if (positional.size() < 3) { std::fprintf(stderr, "usage: set-master <output> <0-200>\n"); return 1; }
        int vol = toInt(positional[2]);
        if (!ctx.post("/api/outputs/master", {{"name", positional[1]}, {"volume", vol}})) { std::fprintf(stderr, "failed\n"); return 1; }
        std::printf("output '%s' master -> %d%%\n", positional[1].c_str(), vol);
        return 0;
    }

    if (cmd == "mute") {
        if (positional.size() < 2) { std::fprintf(stderr, "usage: mute <group> [on|off|toggle]\n"); return 1; }
        auto g = ctx.findGroup(positional[1]);
        if (!g) { std::fprintf(stderr, "group not found: %s\n", positional[1].c_str()); return 1; }
        bool muted;
        if (positional.size() < 3) muted = !g->value("muted", false);
        else if (positional[2] == "toggle") muted = !g->value("muted", false);
        else muted = parseBool(positional[2]);
        if (!ctx.patch("/api/groups/" + std::to_string(g->value("id", 0u)), {{"muted", muted}})) { std::fprintf(stderr, "failed\n"); return 1; }
        std::printf("group '%s' muted -> %s\n", positional[1].c_str(), muted ? "yes" : "no");
        return 0;
    }

    if (cmd == "output-mute") {
        if (positional.size() < 2) { std::fprintf(stderr, "usage: output-mute <output> [on|off|toggle]\n"); return 1; }
        if (!ctx.fetchState()) { std::fprintf(stderr, "API not reachable\n"); return 1; }
        bool muted;
        bool found = false;
        bool current = false;
        for (const auto& o : ctx.state["outputs"]) {
            if (o.value("name", std::string{}) == positional[1]) { current = o.value("muted", false); found = true; break; }
        }
        if (!found) { std::fprintf(stderr, "output not found: %s\n", positional[1].c_str()); return 1; }
        if (positional.size() < 3) muted = !current;
        else if (positional[2] == "toggle") muted = !current;
        else muted = parseBool(positional[2]);
        if (!ctx.post("/api/outputs/master", {{"name", positional[1]}, {"muted", muted}})) { std::fprintf(stderr, "failed\n"); return 1; }
        std::printf("output '%s' muted -> %s\n", positional[1].c_str(), muted ? "yes" : "no");
        return 0;
    }

    if (cmd == "set-direction") {
        if (positional.size() < 3) { std::fprintf(stderr, "usage: set-direction <input> <azimuth> [elevation]\n"); return 1; }
        auto in = ctx.findInput(positional[1]);
        if (!in) { std::fprintf(stderr, "input not found: %s\n", positional[1].c_str()); return 1; }
        double az = std::atof(positional[2].c_str());
        double el = positional.size() > 3 ? std::atof(positional[3].c_str()) : in->value("elevation", 0.0);
        if (!ctx.post("/api/inputs/" + std::to_string(in->value("id", 0u)) + "/direction",
                      {{"azimuth", az}, {"elevation", el}})) { std::fprintf(stderr, "failed\n"); return 1; }
        std::printf("input '%s' direction -> az=%.1f el=%.1f\n", positional[1].c_str(), az, el);
        return 0;
    }

    if (cmd == "set-hrtf") {
        if (positional.size() < 3) { std::fprintf(stderr, "usage: set-hrtf <input> <sofa-path>\n"); return 1; }
        auto in = ctx.findInput(positional[1]);
        if (!in) { std::fprintf(stderr, "input not found: %s\n", positional[1].c_str()); return 1; }
        if (!ctx.patch("/api/inputs/" + std::to_string(in->value("id", 0u)),
                       {{"hrtfPath", positional[2]}})) { std::fprintf(stderr, "failed\n"); return 1; }
        std::printf("input '%s' hrtfPath -> %s\n", positional[1].c_str(), positional[2].c_str());
        return 0;
    }

    if (cmd == "apply-scene") {
        if (positional.size() < 2) { std::fprintf(stderr, "usage: apply-scene <name>\n"); return 1; }
        if (!ctx.post("/api/scenes/apply", {{"name", positional[1]}})) { std::fprintf(stderr, "failed\n"); return 1; }
        std::printf("scene '%s' applied\n", positional[1].c_str());
        return 0;
    }

    if (cmd == "save-scene") {
        if (positional.size() < 2) { std::fprintf(stderr, "usage: save-scene <name>\n"); return 1; }
        if (!ctx.post("/api/scenes/save", {{"name", positional[1]}})) { std::fprintf(stderr, "failed\n"); return 1; }
        std::printf("scene '%s' saved\n", positional[1].c_str());
        return 0;
    }

    if (cmd == "save-preset") {
        if (positional.size() < 2) { std::fprintf(stderr, "usage: save-preset <path>\n"); return 1; }
        if (!ctx.post("/api/presets/save", {{"path", positional[1]}})) { std::fprintf(stderr, "failed\n"); return 1; }
        std::printf("preset saved to '%s'\n", positional[1].c_str());
        return 0;
    }

    if (cmd == "load-preset") {
        if (positional.size() < 2) { std::fprintf(stderr, "usage: load-preset <path>\n"); return 1; }
        if (!ctx.post("/api/presets/load", {{"path", positional[1]}})) { std::fprintf(stderr, "failed\n"); return 1; }
        std::printf("preset loaded from '%s'\n", positional[1].c_str());
        return 0;
    }

    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 1;
}
