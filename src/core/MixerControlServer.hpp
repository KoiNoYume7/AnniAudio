#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace anniaudio::core {

class AudioMixerMatrix;

// Local HTTP + Server-Sent-Events control API for a running AudioMixerMatrix.
// See docs/MIXER-CONTROL-API.md for the full design and endpoint list.
//
// Always binds to 127.0.0.1 only. There is no authentication layer by
// design -- this is meant for a single-user, single-machine tool (the mixer
// GUI and, eventually, a Loupedeck Live plugin), not for exposure beyond
// the local machine. Do not bind this to any other interface.
class MixerControlServer {
public:
    explicit MixerControlServer(AudioMixerMatrix& matrix);
    ~MixerControlServer();

    MixerControlServer(const MixerControlServer&) = delete;
    MixerControlServer& operator=(const MixerControlServer&) = delete;

    // Starts listening on 127.0.0.1:port. Returns false if the port couldn't
    // be bound (e.g. already in use).
    bool start(uint16_t port);
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
