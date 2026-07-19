#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace anniaudio::core {

struct MixerStripConfig {
    std::string name;      // shown in UI / logs (optional)
    std::string source;    // endpoint name hint
    float       volume = 1.0f;
    bool        muted  = false;
};

// Multi-source WASAPI mixer.
// Captures any number of render or capture endpoints and mixes them into a
// single render endpoint with per-strip volume/mute and a master volume.
class AudioMixer {
public:
    AudioMixer();
    ~AudioMixer();

    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    // Open the output endpoint. Must be called before addStrip().
    bool init(const std::string& outputHint);

    // Add a capture/loopback input strip. Returns strip index or -1 on failure.
    int  addStrip(const MixerStripConfig& cfg);

    // Start the mixer thread. Returns false if no output or no strips.
    bool start();

    // Stop and close everything.
    void stop();

    bool running() const noexcept;

    size_t stripCount() const noexcept;

    void setStripVolume(size_t idx, float vol);
    float stripVolume(size_t idx) const;
    void setStripMuted(size_t idx, bool mute);
    bool stripMuted(size_t idx) const;

    void  setMasterVolume(float v);
    float masterVolume() const;

    // Helpers for device naming
    const std::string& outputName() const;
    const std::string& stripName(size_t idx) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace anniaudio::core
