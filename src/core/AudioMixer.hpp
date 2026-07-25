#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio_utils.hpp"

namespace anniaudio::core {

// Opaque, stable strip identifier. Assigned once at addStrip() time and never
// reused or renumbered — safe to hold onto across other strips being added or
// removed, unlike a vector index.
using StripId = uint32_t;

enum class StripSourceType { Device, Application };

struct MixerStripConfig {
    std::string name;      // shown in UI / logs; defaults to `source` if empty
    std::string source;    // endpoint name hint, or "pid" string for application sources
    StripSourceType sourceType = StripSourceType::Device;
    float       volume = 1.0f;
    bool        muted  = false;
    // Capture-side DSP, applied before resampling/mixing so every output of
    // this strip hears the processed signal.
    bool        denoise = false;   // RNNoise suppression (needs 48 kHz capture)
    std::string eqPreset;          // "" = off; "voice" = HPF + mud cut + presence + air
    // HRTF binaural spatialization. When on, the left/right channels are
    // convolved as two virtual speakers (see dsp::Spatializer) and summed into a
    // positioned stereo image. Requires a stereo-or-wider output.
    // azimuth: 0 = front, +90 = left, -90 = right; elevation: 0 = ear level,
    // +90 = above.
    bool        spatial = false;
    float       azimuth = 0.0f;
    float       elevation = 0.0f;
    // Optional binding to a physical controller's Nth control (e.g. a Loupedeck
    // Live knob). Purely informational to AudioMixer itself — it's read back via
    // snapshot() so a control-API client can act on it. 0-based, no fixed range
    // enforced here.
    std::optional<int> knobIndex;
};

struct StripSnapshot {
    StripId             id = 0;
    std::string         name;
    std::string         source;
    std::string         output; // name of the mixer output this strip feeds
    float               volume = 1.0f;
    bool                muted  = false;
    std::optional<int>  knobIndex;
    bool                spatial = false;
    float               azimuth = 0.0f;
    float               elevation = 0.0f;
    float               peak = 0.0f; // post-fader peak level (0..1)
    float               rms  = 0.0f; // post-fader RMS level (0..1)
};

// Multi-source WASAPI mixer.
//
// Captures any number of render (loopback) or capture endpoints and mixes them
// into a single render endpoint with per-strip volume/mute and a master volume.
//
// Thread safety:
//   - init() / start() / stop() are not safe to call concurrently with each
//     other or with anything else; call them from one thread, in order.
//   - Once running, addStrip/removeStrip/renameStrip/setStripKnobIndex and
//     setStripVolume/setStripMuted/snapshot/stripSnapshot/listEndpoints are
//     all safe to call from any thread (e.g. HTTP request handlers), including
//     concurrently with each other and with the mixer's own audio thread.
//     See AudioMixer.cpp for the locking model and why it can't glitch audio.
//   - Any thread calling into AudioMixer that also talks to WASAPI/MMDevice
//     APIs directly (addStrip resolves a device by name) must have called
//     CoInitializeEx(nullptr, COINIT_MULTITHREADED) on itself first.
class AudioMixer {
public:
    AudioMixer();
    ~AudioMixer();

    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    // Open the output endpoint. Must be called before addStrip()/start().
    bool init(const std::string& outputHint);

    // Start the mixer thread. Returns false if init() wasn't called or there
    // are no strips yet (a mixer with nothing to mix is a no-op that would
    // just look like it silently failed).
    bool start();

    // Stop and close everything. Safe to call multiple times.
    void stop();

    bool running() const noexcept;

    // --- Structural changes: safe to call before or after start() ---

    // Resolves `cfg.source` and opens it. Returns the new strip's id, or
    // nullopt if the source couldn't be found/opened or the strip limit (62,
    // bounded by WaitForMultipleObjects) was reached.
    std::optional<StripId> addStrip(const MixerStripConfig& cfg);
    bool removeStrip(StripId id);
    bool renameStrip(StripId id, const std::string& name);
    bool setStripKnobIndex(StripId id, std::optional<int> knobIndex);

    // --- Per-strip control: safe to call before or after start() ---

    bool  setStripVolume(StripId id, float vol);
    bool  setStripMuted(StripId id, bool muted);
    // Live HRTF direction change for a spatialized strip. Cheap and glitch-free
    // (applied on the audio thread between blocks); no-op if the strip isn't
    // spatialized. Safe to call at knob-turn rates.
    bool  setStripDirection(StripId id, float azimuth, float elevation);

    void  setMasterVolume(float v);
    float masterVolume() const;
    void  setMasterMuted(bool muted);
    bool  masterMuted() const;
    float masterPeak() const; // post-master peak level (0..1)
    float masterRms() const;  // post-master RMS level (0..1)

    size_t stripCount() const noexcept;
    std::vector<StripSnapshot>   snapshot() const;
    std::optional<StripSnapshot> stripSnapshot(StripId id) const;

    // Live WASAPI endpoints, for a source picker. Requires the calling
    // thread to be COM-initialized (see class comment above).
    std::vector<EndpointInfo> listEndpoints() const;

    const std::string& outputName() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace anniaudio::core
