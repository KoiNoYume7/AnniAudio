#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace anniaudio::core {

struct HotkeyConfig {
    std::string keys;      // e.g. "Ctrl+Alt+M"
    std::string action;    // toggle_group_mute, nudge_group_volume, toggle_output_mute, nudge_output_volume, set_input_azimuth
    std::string group;     // group name for group actions
    std::string output;    // output name for output actions
    std::string input;     // input name for input actions
    int delta = 0;         // percent or degrees delta
};

// Minimal global hotkey handler for the mixer. Reads a JSON config, registers
// Win32 RegisterHotKey combinations on a hidden window, and dispatches actions
// on a background thread's message loop. The callback runs on that thread.
class GlobalHotkeys {
public:
    using Callback = std::function<void(const HotkeyConfig&)>;

    GlobalHotkeys();
    ~GlobalHotkeys();

    GlobalHotkeys(const GlobalHotkeys&) = delete;
    GlobalHotkeys& operator=(const GlobalHotkeys&) = delete;

    bool load(const std::string& path);
    bool start(Callback cb);
    void stop();
    bool running() const;

    const std::vector<HotkeyConfig>& config() const { return _config; }

private:
    class Impl;
    std::unique_ptr<Impl> p;
    std::vector<HotkeyConfig> _config;
};

} // namespace anniaudio::core
