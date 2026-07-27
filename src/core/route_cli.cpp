#include "AudioEngine.hpp"
#include "AudioMixerMatrix.hpp"
#include "MixerControlServer.hpp"
#include "eq.hpp"
#include "midi_input.hpp"
#include "noise_suppressor.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <propidl.h>
#include <functiondiscoverykeys_devpkey.h>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using namespace anniaudio::dsp;

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
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
    std::printf("  %s route <capture> <render> [volume]  Route any capture endpoint to any render endpoint\n", prog);
    std::printf("  %s process [<source> <out> [vol]] [--preset <file>] [--rnnoise] [--config <profile>]\n", prog);
    std::printf("                                                                    Loopback capture + optional EQ / NR / profile\n", prog);
    std::printf("  %s mixer <config.json> [--port <n>]                               Multi-source mixer with per-strip volume/mute (control API on 127.0.0.1:8850)\n", prog);
    std::printf("  %s midi [list|<device-hint>]                                      List MIDI inputs or monitor messages\n", prog);
    std::printf("  %s process-eq <source> <out> [vol]                                Same as process, but applies a hardcoded test EQ chain\n", prog);
    std::printf("  %s default <render_device>            Set default playback device to the matching endpoint\n", prog);
    std::printf("\nRouting volume commands while running: + or = louder, - quieter, v <0-100> set, q stop.\n");
    std::printf("\nExamples:\n");
    std::printf("  %s list\n", prog);
    std::printf("  %s process --config config/profiles/default.json\n", prog);
    std::printf("  %s process \"Speakers\" \"Headphones\" 80 --preset config/presets/headphones.json --rnnoise\n", prog);
    std::printf("  %s mixer config/mixers/default.json\n", prog);
    std::printf("  %s mixer config/mixers/loupedeck.json\n", prog);
    std::printf("  %s midi list\n", prog);
    std::printf("  %s midi Loupedeck\n", prog);
    std::printf("  %s route \"Studio Main\" \"Headphones\" 50 -- listen to a cable on headphones\n", prog);
    std::printf("  %s default \"Headphones (Crusher ANC 2)\" -- restore default output\n", prog);
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

static void printVolume(float v)
{
    std::printf("[route] Volume: %.0f%%\n", v * 100.0f);
}

static void inputThread(AudioEngine* engine, std::atomic<bool>* stop)
{
    std::string line;
    while (!stop->load() && std::getline(std::cin, line)) {
        if (line.empty() || line == "q" || line == "Q") {
            stop->store(true);
            break;
        }
        float v = engine->getVolume();
        if (line == "+" || line == "=") {
            v += 0.05f;
            if (v > 2.0f) v = 2.0f;
            engine->setVolume(v);
            printVolume(v);
        } else if (line == "-") {
            v -= 0.05f;
            if (v < 0.0f) v = 0.0f;
            engine->setVolume(v);
            printVolume(v);
        } else if ((line.size() > 1) && (line[0] == 'v' || line[0] == 'V')) {
            int pct = std::atoi(line.c_str() + 1);
            if (pct < 0) pct = 0;
            if (pct > 200) pct = 200;
            engine->setVolume(pct / 100.0f);
            printVolume(engine->getVolume());
        } else {
            std::printf("[route] Unknown command '%s'. Use +, -, v <0-100>, or q.\n", line.c_str());
        }
    }
}

static bool parseFilterType(const std::string& s, FilterType* out)
{
    std::string t;
    t.reserve(s.size());
    for (char c : s) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (t == "peak")       { *out = FilterType::Peak;       return true; }
    if (t == "lowshelf")   { *out = FilterType::LowShelf;   return true; }
    if (t == "highshelf")  { *out = FilterType::HighShelf;  return true; }
    if (t == "lowpass")    { *out = FilterType::LowPass;    return true; }
    if (t == "highpass")   { *out = FilterType::HighPass;   return true; }
    if (t == "notch")      { *out = FilterType::Notch;      return true; }
    if (t == "allpass")    { *out = FilterType::Allpass;    return true; }
    return false;
}

static bool loadEqPreset(EqChain& eq, const std::string& path)
{
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "[route] Could not open EQ preset: %s\n", path.c_str());
        return false;
    }

    try {
        nlohmann::json j;
        f >> j;

        const auto& bands = j.at("bands");
        for (const auto& b : bands) {
            std::string typeStr = b.at("type");
            FilterType type;
            if (!parseFilterType(typeStr, &type)) {
                std::fprintf(stderr, "[route] Unknown filter type: %s\n", typeStr.c_str());
                return false;
            }

            double freq    = b.value("freq",    1000.0);
            double gainDb  = b.value("gain_db", 0.0);
            double q       = b.value("q",       1.0);

            eq.addBand(type, freq, gainDb, q);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[route] Failed to parse EQ preset '%s': %s\n", path.c_str(), e.what());
        return false;
    }

    return true;
}

static int cmdRoute(const std::string& captureHint, const std::string& renderHint,
                    float startVolume = 1.0f,
                    const std::string& eqPresetPath = "",
                    bool useRnnoise = false)
{
    AudioEngine engine;
    std::unique_ptr<EqChain> eq;
    std::unique_ptr<NoiseSuppressor> ns;

    if (useRnnoise) {
        ns = std::make_unique<NoiseSuppressor>();
    }
    if (!eqPresetPath.empty()) {
        eq = std::make_unique<EqChain>();
        if (!loadEqPreset(*eq, eqPresetPath)) {
            return 1;
        }
        std::printf("[route] Loaded EQ preset '%s' (%zu bands)\n", eqPresetPath.c_str(), eq->bandCount());
    }

    if (eq || ns) {
        engine.setProcessCallback([useEq = !!eq, useNr = !!ns,
                                   &eq, &ns, &engine](float* buf, uint32_t frames, uint32_t ch) {
            if (useNr) {
                // RNNoise is trained on 48 kHz. Running it on other rates silently
                // misinterprets the time/frequency scale.
                if (engine.captureSampleRate() != 48000) {
                    static bool warned = false;
                    if (!warned) {
                        std::fprintf(stderr, "[route] RNNoise requires a 48 kHz capture source; skipping.\n");
                        warned = true;
                    }
                } else {
                    if (!ns->prepared()) ns->prepare(ch);
                    ns->process(buf, frames, ch);
                }
            }
            if (useEq) {
                if (!eq->prepared()) {
                    eq->prepare(engine.captureSampleRate(), ch);
                }
                eq->process(buf, frames, ch);
            }
        });
    }

    std::printf("[route] Starting:  capture = \"%s\"  →  render = \"%s\"\n",
                captureHint.c_str(), renderHint.c_str());

    if (!engine.start(captureHint, renderHint)) {
        std::fprintf(stderr, "[route] Failed to start route.\n");
        return 1;
    }

    engine.setVolume(startVolume);
    printVolume(startVolume);

    std::atomic<bool> stop{false};
    std::thread input(inputThread, &engine, &stop);
    std::printf("[route] Running. Commands: +/= louder, - quieter, v <0-100>, q/Enter stop.\n");

    while (!stop.load()) {
        Sleep(100);
    }

    engine.stop();
    if (input.joinable()) input.join();
    std::printf("[route] Stopped.\n");
    return 0;
}

static int cmdProcessEq(const std::string& sourceHint, const std::string& renderHint, float startVolume = 1.0f)
{
    AudioEngine engine;
    EqChain eq;

    // Default "can you hear it" preset: high-pass rumble, small mid boost, gentle air shelf.
    eq.addBand(FilterType::HighPass, 80.0, 0.0, 0.707);
    eq.addBand(FilterType::Peak, 1500.0, 6.0, 1.0);
    eq.addBand(FilterType::HighShelf, 12000.0, 3.0, 0.707);

    engine.setProcessCallback([&eq, &engine](float* buf, uint32_t frames, uint32_t ch) {
        if (!eq.prepared()) {
            eq.prepare(engine.captureSampleRate(), ch);
        }
        eq.process(buf, frames, ch);
    });

    std::printf("[route] Starting EQ process:  source = \"%s\"  →  render = \"%s\"\n",
                sourceHint.c_str(), renderHint.c_str());

    if (!engine.start(sourceHint, renderHint)) {
        std::fprintf(stderr, "[route] Failed to start EQ process.\n");
        return 1;
    }

    engine.setVolume(startVolume);
    printVolume(startVolume);

    std::atomic<bool> stop{false};
    std::thread input(inputThread, &engine, &stop);
    std::printf("[route] Running EQ. Commands: +/= louder, - quieter, v <0-100>, q/Enter stop.\n");

    while (!stop.load()) {
        Sleep(100);
    }

    engine.stop();
    if (input.joinable()) input.join();
    std::printf("[route] Stopped.\n");
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

    AudioEngine engine;
    auto endpoints = engine.listEndpoints();
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
    HRESULT hr = CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL,
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

// ---------------------------------------------------------------------------
// Process profile loading
// ---------------------------------------------------------------------------
struct ProcessProfile {
    std::string source;
    std::string output;
    float       volume    = 1.0f;
    std::string preset;
    bool        rnnoise   = false;
};

static bool loadProcessProfile(const std::string& path, ProcessProfile& out)
{
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "[route] Could not open profile: %s\n", path.c_str());
        return false;
    }

    try {
        nlohmann::json j;
        f >> j;

        out.source  = j.value("source", out.source);
        out.output  = j.value("output", out.output);
        out.volume  = j.value("volume", out.volume * 100.0f) / 100.0f; // accept 0-100 or 0.0-1.0
        out.preset  = j.value("preset", out.preset);
        out.rnnoise = j.value("rnnoise", out.rnnoise);

        // Clamp volume to a sane range.
        if (out.volume < 0.0f) out.volume = 0.0f;
        if (out.volume > 2.0f) out.volume = 2.0f;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[route] Failed to parse profile '%s': %s\n", path.c_str(), e.what());
        return false;
    }

    return true;
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

// Interactive commands (v/m) address strips by their 1-based position in the
// most recent listing, for convenience at a terminal. That position is
// resolved to the strip's real StripId right here, at the moment the command
// is typed -- it is not held onto, so it can't go stale like a GUI holding a
// raw index across multiple actions would.
static void printMixerHelp(anniaudio::core::AudioMixerMatrix& matrix)
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

    anniaudio::core::AudioMixerMatrix matrix;
    matrix.setAutosavePath(configPath);

    if (!matrix.load(configPath)) {
        std::fprintf(stderr, "[mixer] Failed to load config '%s'\n", configPath.c_str());
        CoUninitialize();
        return 1;
    }
    matrix.setControlPort(controlPort);

    std::unique_ptr<anniaudio::core::MixerControlServer> controlServer;
    if (controlPort != 0) {
        controlServer = std::make_unique<anniaudio::core::MixerControlServer>(matrix);
        controlServer->setAutosavePath(configPath);
        if (!controlServer->start(controlPort)) {
            std::fprintf(stderr, "[mixer] Warning: control API could not bind port %u; continuing without it\n", controlPort);
            controlServer.reset();
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
            anniaudio::core::GroupId gid = target.id;

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
    else if (cmd == "process") {
        ProcessProfile profile;
        bool hasSource = false, hasOutput = false, hasVolume = false;

        for (int i = 2; i < argc; ) {
            std::string a = argv[i];
            std::string al;
            al.reserve(a.size());
            for (char ch : a) al.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));

            if (al == "--config" || al == "-c") {
                if (i + 1 >= argc) { std::fprintf(stderr, "Expected path after %s\n", a.c_str()); return 1; }
                if (!loadProcessProfile(argv[i + 1], profile)) return 1;
                i += 2;
            } else if (al == "--preset" || al == "-p") {
                if (i + 1 >= argc) { std::fprintf(stderr, "Expected path after %s\n", a.c_str()); return 1; }
                profile.preset = argv[i + 1];
                i += 2;
            } else if (al == "--rnnoise" || al == "-n") {
                profile.rnnoise = true;
                i += 1;
            } else if (al == "--no-rnnoise") {
                profile.rnnoise = false;
                i += 1;
            } else if (al[0] == '-') {
                std::fprintf(stderr, "Unknown option: %s\n", a.c_str());
                return 1;
            } else {
                // Positional arguments in order: source, output, volume
                if (!hasSource) {
                    profile.source = a; hasSource = true;
                } else if (!hasOutput) {
                    profile.output = a; hasOutput = true;
                } else if (!hasVolume) {
                    profile.volume = std::atoi(a.c_str()) / 100.0f;
                    if (profile.volume < 0.0f) profile.volume = 0.0f;
                    if (profile.volume > 2.0f) profile.volume = 2.0f;
                    hasVolume = true;
                } else {
                    std::fprintf(stderr, "Usage: process <source> <output> [vol] [--preset <file>] [--rnnoise] [--config <profile>]\n");
                    return 1;
                }
                i += 1;
            }
        }

        if (profile.source.empty() || profile.output.empty()) {
            std::fprintf(stderr, "Usage: process <source> <output> [vol] [--preset <file>] [--rnnoise] [--config <profile>]\n");
            return 1;
        }

        return cmdRoute(profile.source, profile.output, profile.volume, profile.preset, profile.rnnoise);
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
    else if (cmd == "process-eq") {
        if (argc < 4) { std::fprintf(stderr, "Usage: process-eq <loopback_source> <render_output> [volume%%]\n"); return 1; }
        float vol = (argc >= 5) ? std::atoi(argv[4]) / 100.0f : 1.0f;
        return cmdProcessEq(argv[2], argv[3], vol);
    }
    else if (cmd == "route") {
        if (argc < 4) { std::fprintf(stderr, "Usage: route <capture_name> <render_name> [volume%%]\n"); return 1; }
        float vol = (argc >= 5) ? std::atoi(argv[4]) / 100.0f : 1.0f;
        return cmdRoute(argv[2], argv[3], vol);
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
