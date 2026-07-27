#include "global_hotkeys.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#include <windows.h>

namespace anniaudio::core {

namespace {

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

UINT parseVk(const std::string& key)
{
    std::string k = toLower(key);
    if (k.empty()) return 0;

    // Letters and digits
    if (k.size() == 1 && k[0] >= 'a' && k[0] <= 'z') return 'A' + (k[0] - 'a');
    if (k.size() == 1 && k[0] >= '0' && k[0] <= '9') return '0' + (k[0] - '0');

    static const std::unordered_map<std::string, UINT> map = {
        {"space", VK_SPACE},
        {"tab", VK_TAB},
        {"enter", VK_RETURN},
        {"return", VK_RETURN},
        {"esc", VK_ESCAPE},
        {"escape", VK_ESCAPE},
        {"backspace", VK_BACK},
        {"delete", VK_DELETE},
        {"del", VK_DELETE},
        {"insert", VK_INSERT},
        {"home", VK_HOME},
        {"end", VK_END},
        {"pageup", VK_PRIOR},
        {"pagedown", VK_NEXT},
        {"up", VK_UP},
        {"down", VK_DOWN},
        {"left", VK_LEFT},
        {"right", VK_RIGHT},
        {"plus", VK_OEM_PLUS},
        {"minus", VK_OEM_MINUS},
        {"add", VK_ADD},
        {"subtract", VK_SUBTRACT},
        {"kp_plus", VK_ADD},
        {"kp_minus", VK_SUBTRACT},
        {"volume_mute", VK_VOLUME_MUTE},
        {"volume_down", VK_VOLUME_DOWN},
        {"volume_up", VK_VOLUME_UP},
        {"media_next", VK_MEDIA_NEXT_TRACK},
        {"media_prev", VK_MEDIA_PREV_TRACK},
        {"media_stop", VK_MEDIA_STOP},
        {"media_play", VK_MEDIA_PLAY_PAUSE},
        {"media_play_pause", VK_MEDIA_PLAY_PAUSE},
    };

    auto it = map.find(k);
    if (it != map.end()) return it->second;

    // F1-F24
    if (k.size() > 1 && k[0] == 'f') {
        int n = 0;
        try { n = std::stoi(k.substr(1)); } catch (...) {}
        if (n >= 1 && n <= 24) return VK_F1 + (n - 1);
    }

    return 0;
}

bool parseKeyCombo(const std::string& combo, UINT& vk, UINT& modifiers)
{
    vk = 0;
    modifiers = 0;

    std::string s = toLower(combo);
    std::replace(s.begin(), s.end(), '+', ' ');
    std::replace(s.begin(), s.end(), '-', ' ');

    std::istringstream iss(s);
    std::string token;
    while (iss >> token) {
        if (token == "ctrl" || token == "control") modifiers |= MOD_CONTROL;
        else if (token == "alt") modifiers |= MOD_ALT;
        else if (token == "shift") modifiers |= MOD_SHIFT;
        else if (token == "win" || token == "windows" || token == "mod") modifiers |= MOD_WIN;
        else {
            UINT v = parseVk(token);
            if (v == 0) {
                std::fprintf(stderr, "[GlobalHotkeys] unknown key '%s' in combo '%s'\n", token.c_str(), combo.c_str());
                return false;
            }
            if (vk != 0) {
                std::fprintf(stderr, "[GlobalHotkeys] multiple non-modifier keys in '%s'\n", combo.c_str());
                return false;
            }
            vk = v;
        }
    }

    return vk != 0;
}

} // namespace

class GlobalHotkeys::Impl {
public:
    ~Impl() { stop(); }

    bool load(const std::string& path, std::vector<HotkeyConfig>& config)
    {
        config.clear();
        std::ifstream f(path);
        if (!f) return false;

        nlohmann::json j;
        try { f >> j; } catch (const std::exception& e) {
            std::fprintf(stderr, "[GlobalHotkeys] failed to parse '%s': %s\n", path.c_str(), e.what());
            return false;
        }

        if (j.contains("hotkeys") && j["hotkeys"].is_array()) {
            for (const auto& h : j["hotkeys"]) {
                HotkeyConfig cfg;
                cfg.keys   = h.value("keys", std::string{});
                cfg.action = h.value("action", std::string{});
                cfg.group  = h.value("group", std::string{});
                cfg.output = h.value("output", std::string{});
                cfg.input  = h.value("input", std::string{});
                cfg.delta  = h.value("delta", 0);
                if (!cfg.keys.empty() && !cfg.action.empty()) config.push_back(std::move(cfg));
            }
        }
        return true;
    }

    bool start(const std::vector<HotkeyConfig>& config, Callback cb)
    {
        if (running_.exchange(true)) return false;
        if (config.empty()) {
            running_ = false;
            return false;
        }

        callback_ = std::move(cb);

        WNDCLASSEX wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Impl::wndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"AnniAudioHotkeys";
        RegisterClassEx(&wc);

        hwnd_ = CreateWindowEx(0, L"AnniAudioHotkeys", L"AnniAudio Hotkeys", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
        if (!hwnd_) {
            running_ = false;
            return false;
        }

        SetWindowLongPtr(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        int id = 1;
        for (const auto& cfg : config) {
            UINT vk = 0, mods = 0;
            if (parseKeyCombo(cfg.keys, vk, mods)) {
                if (RegisterHotKey(hwnd_, id, mods, vk)) {
                    hotkeys_[id] = &cfg;
                } else {
                    std::fprintf(stderr, "[GlobalHotkeys] could not register '%s' (0x%X / 0x%X)\n", cfg.keys.c_str(), mods, vk);
                }
            }
            ++id;
        }

        thread_ = std::thread([this]() { this->run(); });
        return true;
    }

    void stop()
    {
        if (!running_.exchange(false)) return;
        if (hwnd_) {
            PostMessage(hwnd_, WM_CLOSE, 0, 0);
        }
        if (thread_.joinable()) thread_.join();
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        UnregisterClass(L"AnniAudioHotkeys", GetModuleHandle(nullptr));
    }

    bool isRunning() const { return running_.load(); }

private:
    void run()
    {
        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProc(hwnd, msg, wParam, lParam);

        if (msg == WM_HOTKEY) {
            int id = static_cast<int>(wParam);
            auto it = self->hotkeys_.find(id);
            if (it != self->hotkeys_.end() && self->callback_) {
                self->callback_(*it->second);
            }
            return 0;
        }

        if (msg == WM_CLOSE || msg == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    std::atomic<bool> running_{false};
    HWND hwnd_ = nullptr;
    std::thread thread_;
    Callback callback_;
    std::unordered_map<int, const HotkeyConfig*> hotkeys_;
};

GlobalHotkeys::GlobalHotkeys() : p(std::make_unique<Impl>()) {}
GlobalHotkeys::~GlobalHotkeys() = default;

bool GlobalHotkeys::load(const std::string& path)
{
    return p->load(path, _config);
}

bool GlobalHotkeys::start(Callback cb)
{
    return p->start(_config, std::move(cb));
}

void GlobalHotkeys::stop()
{
    p->stop();
}

bool GlobalHotkeys::running() const
{
    return p && p->isRunning();
}

} // namespace anniaudio::core
