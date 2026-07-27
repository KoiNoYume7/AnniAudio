#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace anniaudio::core {

class AudioMixerMatrix;

// Local HTTP + Server-Sent-Events (+ optional WebSocket) control API for a
// running AudioMixerMatrix. See docs/MIXER-CONTROL-API.md for the full design
// and endpoint list.
//
// Defaults to loopback-only (127.0.0.1) with no authentication. Binding to any
// other interface should always be paired with a non-empty apiKey.
class MixerControlServer {
public:
    explicit MixerControlServer(AudioMixerMatrix& matrix);
    ~MixerControlServer();

    MixerControlServer(const MixerControlServer&) = delete;
    MixerControlServer& operator=(const MixerControlServer&) = delete;

    // Starts listening on bindAddress:port. Returns false if the port couldn't
    // be bound (e.g. already in use or invalid address).
    // If apiKey is non-empty, every request must carry an "X-API-Key" header
    // matching it (SSE and WebSocket upgrades included).
    bool start(uint16_t port,
               const std::string& bindAddress = "127.0.0.1",
               const std::string& apiKey = std::string{});
    void stop();

    bool     running() const noexcept { return m_running.load(); }
    uint16_t port() const noexcept { return m_port; }

    void setAutosavePath(const std::string& path);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    std::atomic<bool> m_running{false};
    uint16_t m_port = 0;
};

} // namespace anniaudio::core
