#include "AudioEngine.hpp"

#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

static void printUsage(const char* prog)
{
    std::printf(R"(AnniAudio Routing CLI
Usage:
  %s list                  List all audio endpoints, marking AnniAudio cables
  %s monitor <cable> [out] Route cable CAPTURE to physical RENDER (default: default output)
  %s inject  <in>   <cable> Route physical CAPTURE to cable RENDER
  %s passthrough <in> <out> Route any capture endpoint to any render endpoint

Examples:
  %s list
  %s monitor "Studio Main"      -- listen to cable 1 on your headphones
  %s inject  "Microphone" "Voice Chat" -- send mic to cable 2
)
", prog, prog, prog, prog, prog, prog, prog);
}

static int cmdList()
{
    AudioEngine engine;
    auto endpoints = engine.listEndpoints();

    std::printf("\n%-4s %-8s %-12s %-30s  %s\n", "#", "Flow", "Type", "Name", "Device ID (truncated)");
    std::printf("--------------------------------------------------------------------------------\n");

    int idx = 0;
    for (const auto& ep : endpoints) {
        const char* flow = ep.isRender ? "RENDER" : "CAPTURE";
        const char* type = ep.isAnniAudio ? "[CABLE]" : "[sys]";
        const char* def  = ep.isDefault ? " (default)" : "";
        std::printf("%-4d %-8s %-12s %-30s%s\n  %s\n\n",
                    idx++, flow, type,
                    (ep.name + def).c_str(),
                    "", // spacer
                    ep.id.c_str());
    }
    std::printf("Found %d endpoint(s)\n", idx);
    return 0;
}

static int cmdRoute(const std::string& captureHint, const std::string& renderHint)
{
    AudioEngine engine;
    std::printf("[route] Starting:  capture = \"%s\"  →  render = \"%s\"\n",
                captureHint.c_str(), renderHint.c_str());

    if (!engine.start(captureHint, renderHint)) {
        std::fprintf(stderr, "[route] Failed to start route.\n");
        return 1;
    }

    std::printf("[route] Running. Press Enter to stop...\n");
    std::getchar();

    engine.stop();
    std::printf("[route] Stopped.\n");
    return 0;
}

int main(int argc, char* argv[])
{
    if (argc < 2) { printUsage(argv[0]); return 1; }

    std::string cmd = argv[1];
    for (auto& c : cmd) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (cmd == "list") {
        return cmdList();
    }
    else if (cmd == "monitor") {
        if (argc < 3) { std::fprintf(stderr, "Usage: monitor <cable_name> [output_name]\n"); return 1; }
        std::string cable = argv[2];
        std::string out   = (argc >= 4) ? argv[3] : "";
        // monitor = capture from cable, render to physical output
        return cmdRoute(cable, out);
    }
    else if (cmd == "inject") {
        if (argc < 4) { std::fprintf(stderr, "Usage: inject <input_name> <cable_name>\n"); return 1; }
        std::string in    = argv[2];
        std::string cable = argv[3];
        // inject = capture from physical input, render to cable
        return cmdRoute(in, cable);
    }
    else if (cmd == "passthrough") {
        if (argc < 4) { std::fprintf(stderr, "Usage: passthrough <capture_name> <render_name>\n"); return 1; }
        return cmdRoute(argv[2], argv[3]);
    }
    else {
        std::fprintf(stderr, "Unknown command: %s\n", argv[1]);
        printUsage(argv[0]);
        return 1;
    }
}
