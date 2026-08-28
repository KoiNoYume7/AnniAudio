#include "AudioMixerMatrix.hpp"
#include "MixerControlServer.hpp"
#include "audio_utils.hpp"
#include "global_hotkeys.hpp"
#include "midi_input.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <propidl.h>
#include <functiondiscoverykeys_devpkey.h>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using namespace anniaudio::core;

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Undocumented IPolicyConfig used by Windows to switch default endpoints.
// Method order must match the COM vtable.
struct DeviceShareMode { LONG sharedMode; UINT32 dummy[2]; };
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(LPCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(LPCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(LPCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(LPCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(LPCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(LPCWSTR, struct DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(LPCWSTR, struct DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(LPCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(LPCWSTR, const PROPERTYKEY&, PROPVARIANT) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(LPCWSTR, ERole) = 0;
};
static const CLSID CLSID_PolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e,
    {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}
};

static void printUsage(const char* prog)
{
    std::printf("AnniAudio Routing CLI\n");
    std::printf("Usage:\n");
    std::printf("  %s list                             List all audio endpoints, marking AnniAudio cables\n", prog);
    std::printf("  %s mixer <config.json> [--port <n>]  Multi-source mixer with per-group volume/mute (control API on bindAddress:controlPort)\n", prog);
    std::printf("  %s midi [list|<device-hint>]        List MIDI inputs or monitor messages\n", prog);
    std::printf("  %s default <render_device>           Set default playback device to the matching endpoint\n", prog);
    std::printf("\nMixer commands while running: v <group> <0-200>, m <group>, o <output>, +/=, -, ?, q.\n");
    std::printf("\nExamples:\n");
    std::printf("  %s list\n", prog);
    std::printf("  %s mixer config/mixers/default.json\n", prog);
    std::printf("  %s mixer config/mixers/loupedeck.json\n", prog);
    std::printf("  %s midi list\n", prog);
    std::printf("  %s midi Loupedeck\n", prog);
    std::printf("  %s default \"Speakers\"\n", prog);
}

static int cmdList()
{
    EnsureComInitializedOnThisThread();

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) {
        std::fprintf(stderr, "[list] Failed to create device enumerator: 0x%08X\n", (unsigned)hr);
        return 1;
    }

    auto endpoints = enumEndpoints(enumerator.Get());

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

static std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static int cmdDefault(const std::string& hint)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) {
        std::fprintf(stderr, "[default] Failed to create device enumerator: 0x%08X\n", (unsigned)hr);
        CoUninitialize();
        return 2;
    }

    auto endpoints = enumEndpoints(enumerator.Get());
    const EndpointInfo* target = nullptr;
    for (const auto& ep : endpoints) {
        if (!ep.isRender) continue;
        if (ep.name.empty() || hint.empty()) continue;
        auto it = std::search(ep.name.begin(), ep.name.end(), hint.begin(), hint.end(),
            [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
        if (it != ep.name.end()) { target = &ep; break; }
    }

    if (!target) {
        std::fprintf(stderr, "[default] No render endpoint matches \"%s\"\n", hint.c_str());
        CoUninitialize();
        return 2;
    }

    ComPtr<IPolicyConfig> pc;
    hr = CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(&pc));
    if (FAILED(hr) || !pc) {
        std::fprintf(stderr, "[default] Failed to create IPolicyConfig: 0x%08X\n", (unsigned)hr);
        CoUninitialize();
        return 3;
    }

    std::wstring id = utf8ToWide(target->id);
    hr = pc->SetDefaultEndpoint(id.c_str(), eConsole);
    if (SUCCEEDED(hr)) hr = pc->SetDefaultEndpoint(id.c_str(), eMultimedia);
    if (SUCCEEDED(hr)) hr = pc->SetDefaultEndpoint(id.c_str(), eCommunications);

    if (FAILED(hr)) {
        std::fprintf(stderr, "[default] SetDefaultEndpoint failed: 0x%08X\n", (unsigned)hr);
        CoUninitialize();
        return 3;
    }

    std::printf("[default] Default playback set to: %s\n", target->name.c_str());
    pc.Reset();          // release COM object before tearing down the apartment
    CoUninitialize();
    return 0;
}

static int cmdMidi(const std::string& arg)
{
    if (arg == "list" || arg.empty()) {
        std::printf("MIDI input devices:\n");
        auto devs = anniaudio::midi::MidiInput::listDevices();
        for (size_t i = 0; i < devs.size(); ++i) {
            std::printf("  %zu: %s\n", i, devs[i].c_str());
        }
        if (arg == "list") return 0;
    }

    std::string hint = arg.empty() ? "Loupedeck" : arg;
    std::printf("[midi] Listening on first device matching '%s'. Press q + Enter to stop.\n", hint.c_str());

    anniaudio::midi::MidiInput midi;
    bool opened = midi.open(hint, [](const anniaudio::midi::MidiMessage& msg) {
        std::printf("[midi] status=0x%02X data1=%3u data2=%3u\n", msg.status, msg.data1, msg.data2);
    });
    if (!opened) return 1;

    std::string line;
    while (std::getline(std::cin, line)) {
        size_t pos = line.find_first_not_of(" \t\r\n");
        if (pos != std::string::npos && std::tolower(static_cast<unsigned char>(line[pos])) == 'q') break;
    }
    return 0;
}

// Interactive commands (v/m) address groups by their 1-based position in the
// most recent listing, for convenience at a terminal. That position is
// resolved to the group's real GroupId right here, at the moment the command
// is typed -- it is not held onto, so it can't go stale like a GUI holding a
// raw index across multiple actions would.
static void printMixerHelp(AudioMixerMatrix& matrix)
{
    auto state = matrix.snapshot();
    std::printf("\n[mixer] Controls:\n");
    std::printf("  v <group> <0-200>   set group volume\n");
    std::printf("  m <group>           toggle group mute\n");
    std::printf("  o <output>          select output for master +/-\n");
    std::printf("  + / -               master volume +/- 5%% on selected output\n");
    std::printf("  ?                   print this help\n");
    std::printf("  q                   quit\n\n");

    std::printf("[mixer] Outputs:\n");
    int oidx = 1;
    for (const auto& out : state.outputs) {
        std::printf("  [%d] %-20s (master %.0f%%)\n", oidx++, out.name.c_str(), out.master);
    }

    std::printf("\n[mixer] Groups:\n");
    for (size_t i = 0; i < state.groups.size(); ++i) {
        const auto& g = state.groups[i];
        std::printf("  [%zu] %-20s  %3.0f%%  %s\n", i + 1, g.name.c_str(), g.volume, g.muted ? "MUTED" : "");
        std::printf("      inputs:  ");
        for (auto iid : g.inputIds) {
            for (const auto& in : state.inputs) {
                if (in.id == iid) { std::printf("%s ", in.name.c_str()); break; }
            }
        }
        std::printf("\n      outputs: ");
        for (const auto& on : g.outputIds) std::printf("%s ", on.c_str());
        std::printf("\n");
    }
}

static std::string autosavePathFor(const std::string& configPath)
{
    std::filesystem::path p(configPath);
    std::string fname = p.filename().string();

    // Committed example configs are read-only templates; runtime state should
    // be written to a user config so the examples stay untouched.
    if (fname == "default.json" || fname == "loupedeck.json" || fname == "test_matrix.json") {
        auto mainPath = p.parent_path() / "main.json";
        std::printf("[mixer] Loaded example config %s; autosaving to %s\n",
                    configPath.c_str(), mainPath.string().c_str());
        return mainPath.string();
    }
    return configPath;
}

static int cmdMixer(const std::string& configPath, std::optional<uint16_t> portOverride = std::nullopt)
{
    nlohmann::json j;
    try {
        std::ifstream f(configPath);
        if (!f) { std::fprintf(stderr, "[mixer] Cannot open config '%s'\n", configPath.c_str()); return 1; }
        f >> j;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[mixer] Failed to parse config: %s\n", e.what());
        return 1;
    }

    uint16_t controlPort = portOverride.value_or(static_cast<uint16_t>(j.value("controlPort", 8850)));

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::fprintf(stderr, "[mixer] CoInitializeEx failed 0x%08X\n", (unsigned)hr);
        return 1;
    }

    std::string autosavePath = autosavePathFor(configPath);
    AudioMixerMatrix matrix;
    matrix.setAutosavePath(autosavePath);

    if (!matrix.load(configPath)) {
        std::fprintf(stderr, "[mixer] Failed to load config '%s'\n", configPath.c_str());
        CoUninitialize();
        return 1;
    }
    matrix.setControlPort(controlPort);

    std::string bindAddress = "127.0.0.1";
    std::string apiKey;
    {
        auto snap = matrix.snapshot();
        if (!snap.bindAddress.empty()) bindAddress = snap.bindAddress;
        apiKey = snap.apiKey;
    }

    std::unique_ptr<MixerControlServer> controlServer;
    if (controlPort != 0) {
        controlServer = std::make_unique<MixerControlServer>(matrix);
        controlServer->setAutosavePath(autosavePath);
        if (!controlServer->start(controlPort, bindAddress, apiKey)) {
            std::fprintf(stderr, "[mixer] Warning: control API could not bind %s:%u; continuing without it\n",
                         bindAddress.c_str(), controlPort);
            controlServer.reset();
        } else if (bindAddress != "127.0.0.1" && apiKey.empty()) {
            std::fprintf(stderr, "[mixer] Warning: API is bound to %s with no API key. Set apiKey in the config or use --api-key.\n",
                         bindAddress.c_str());
        }
    }

    // Optional global hotkeys: config/hotkeys.json next to the mixer config.
    std::unique_ptr<GlobalHotkeys> hotkeys;
    {
        std::filesystem::path p(configPath);
        auto hotkeyPath = (p.parent_path().parent_path() / "hotkeys.json").string();
        hotkeys = std::make_unique<GlobalHotkeys>();
        if (hotkeys->load(hotkeyPath)) {
            bool started = hotkeys->start([&matrix](const HotkeyConfig& cfg) {
                auto snap = matrix.snapshot();
                if (cfg.action == "toggle_group_mute" || cfg.action == "mute_group") {
                    for (const auto& g : snap.groups) {
                        if (g.name == cfg.group) { matrix.setGroupMuted(g.id, !g.muted); return; }
                    }
                } else if (cfg.action == "nudge_group_volume") {
                    for (const auto& g : snap.groups) {
                        if (g.name == cfg.group) {
                            float v = std::clamp(g.volume + cfg.delta, 0.0f, 200.0f);
                            matrix.setGroupVolume(g.id, v);
                            return;
                        }
                    }
                } else if (cfg.action == "toggle_output_mute" || cfg.action == "mute_output") {
                    for (const auto& o : snap.outputs) {
                        if (o.name == cfg.output) { matrix.setOutputMuted(o.name, !o.muted); return; }
                    }
                } else if (cfg.action == "nudge_output_volume") {
                    for (const auto& o : snap.outputs) {
                        if (o.name == cfg.output) {
                            float v = std::clamp(o.master + cfg.delta, 0.0f, 200.0f);
                            matrix.setOutputMasterVolume(o.name, v);
                            return;
                        }
                    }
                } else if (cfg.action == "nudge_input_azimuth") {
                    for (const auto& in : snap.inputs) {
                        if (in.name == cfg.input) {
                            matrix.setInputDirection(in.id, in.azimuth + cfg.delta, in.elevation);
                            return;
                        }
                    }
                } else if (cfg.action == "set_input_direction") {
                    for (const auto& in : snap.inputs) {
                        if (in.name == cfg.input) {
                            matrix.setInputDirection(in.id, static_cast<float>(cfg.delta), in.elevation);
                            return;
                        }
                    }
                }
            });
            if (started) std::printf("[mixer] Global hotkeys loaded from %s\n", hotkeyPath.c_str());
        } else {
            hotkeys.reset();
        }
    }

    std::printf("[mixer] Running. Commands: v <group> <vol>, m <group>, o <out>, +/=, -, ?, q\n");
    printMixerHelp(matrix);

    auto state = matrix.snapshot();
    std::string selectedOutput = state.outputs.empty() ? "" : state.outputs[0].name;
    std::string line;
    while (std::getline(std::cin, line)) {
        size_t pos = line.find_first_not_of(" \t\r\n");
        if (pos == std::string::npos) continue;
        std::string a = line.substr(pos);
        if (a.empty()) continue;

        char cmdChar = static_cast<char>(std::tolower(static_cast<unsigned char>(a[0])));
        if (cmdChar == 'q') break;

        if (cmdChar == '?') {
            printMixerHelp(matrix);
            continue;
        }

        if (cmdChar == 'o') {
            auto snap = matrix.snapshot();
            std::stringstream ss(a.substr(1));
            int idx = 0; ss >> idx;
            if (idx < 1 || idx > static_cast<int>(snap.outputs.size())) {
                std::fprintf(stderr, "[mixer] Output index must be between 1 and %zu\n", snap.outputs.size());
            } else {
                selectedOutput = snap.outputs[idx - 1].name;
                std::printf("[mixer] Selected output: %s\n", selectedOutput.c_str());
            }
            continue;
        }

        if (cmdChar == '+' || cmdChar == '=') {
            if (selectedOutput.empty()) {
                std::fprintf(stderr, "[mixer] No output selected (use 'o <n>')\n"); continue;
            }
            float v = matrix.outputMasterVolume(selectedOutput) + 5.0f;
            if (v > 200.0f) v = 200.0f;
            matrix.setOutputMasterVolume(selectedOutput, v);
            std::printf("[mixer] %s master: %.0f%%\n", selectedOutput.c_str(), v);
            continue;
        }
        if (cmdChar == '-') {
            if (selectedOutput.empty()) {
                std::fprintf(stderr, "[mixer] No output selected (use 'o <n>')\n"); continue;
            }
            float v = matrix.outputMasterVolume(selectedOutput) - 5.0f;
            if (v < 0.0f) v = 0.0f;
            matrix.setOutputMasterVolume(selectedOutput, v);
            std::printf("[mixer] %s master: %.0f%%\n", selectedOutput.c_str(), v);
            continue;
        }

        if (cmdChar == 'm' || cmdChar == 'v') {
            auto snap = matrix.snapshot();
            std::stringstream ss(a.substr(1));
            int idx = 0; ss >> idx;
            if (idx < 1 || (size_t)idx > snap.groups.size()) {
                std::fprintf(stderr, "[mixer] Group index must be between 1 and %zu\n", snap.groups.size());
                continue;
            }
            const auto& target = snap.groups[idx - 1];
            GroupId gid = target.id;

            if (cmdChar == 'm') {
                bool mute = !target.muted;
                matrix.setGroupMuted(gid, mute);
                std::printf("[mixer] [%d] %s\n", idx, mute ? "MUTED" : "unmuted");
            } else {
                int volInt = 0; ss >> volInt;
                float vol = static_cast<float>(volInt);
                if (vol < 0.0f) vol = 0.0f;
                if (vol > 200.0f) vol = 200.0f;
                matrix.setGroupVolume(gid, vol);
                std::printf("[mixer] [%d] volume %.0f%%\n", idx, vol);
            }
            continue;
        }

        std::fprintf(stderr, "[mixer] Unknown command: %s (try ?)\n", a.c_str());
    }

    std::printf("[mixer] Stopping...\n");
    if (controlServer) controlServer->stop();
    if (hotkeys) hotkeys->stop();
    matrix.stop();
    CoUninitialize();
    std::printf("[mixer] Stopped.\n");
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
    else if (cmd == "mixer") {
        if (argc < 3) { std::fprintf(stderr, "Usage: mixer <config.json> [--port <n>]\n"); return 1; }
        std::string configPath;
        std::optional<uint16_t> portOverride;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            std::string al;
            al.reserve(a.size());
            for (char ch : a) al.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            if (al == "--port" || al == "-p") {
                if (i + 1 >= argc) { std::fprintf(stderr, "Expected port number after %s\n", a.c_str()); return 1; }
                int p = std::atoi(argv[i + 1]);
                if (p < 0 || p > 65535) { std::fprintf(stderr, "Port must be 0-65535\n"); return 1; }
                portOverride = static_cast<uint16_t>(p);
                ++i;
            } else if (al[0] == '-') {
                std::fprintf(stderr, "Unknown option: %s\n", a.c_str());
                return 1;
            } else {
                if (!configPath.empty()) { std::fprintf(stderr, "Usage: mixer <config.json> [--port <n>]\n"); return 1; }
                configPath = a;
            }
        }
        if (configPath.empty()) { std::fprintf(stderr, "Usage: mixer <config.json> [--port <n>]\n"); return 1; }
        return cmdMixer(configPath, portOverride);
    }
    else if (cmd == "midi") {
        std::string arg = (argc >= 3) ? argv[2] : "list";
        return cmdMidi(arg);
    }
    else if (cmd == "default") {
        if (argc < 3) { std::fprintf(stderr, "Usage: default <render_device_name>\n"); return 1; }
        return cmdDefault(argv[2]);
    }
    else {
        std::fprintf(stderr, "Unknown command: %s\n", argv[1]);
        printUsage(argv[0]);
        return 1;
    }
}
