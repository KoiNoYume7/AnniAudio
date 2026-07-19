#pragma once

#include "AudioMixer.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace anniaudio::core {

// A routing matrix: zero or more output mixers, each of which can have any
// number of input sources connected to it.  A single source can be routed to
// multiple outputs; each route has independent volume/mute.
//
// Internally this currently owns one AudioMixer per output.  Sources routed to
// multiple outputs are opened once per output; this is simple and matches the
// existing per-output mixer design.  A future optimisation can share capture
// across outputs if that becomes necessary.
class AudioMixerMatrix {
public:
    using RouteId = uint64_t;

    AudioMixerMatrix();
    ~AudioMixerMatrix();

    AudioMixerMatrix(const AudioMixerMatrix&) = delete;
    AudioMixerMatrix& operator=(const AudioMixerMatrix&) = delete;

    // Add/remove output endpoints. addOutput() opens the device but does not
    // start it.  Start happens automatically when the first route is added.
    bool addOutput(const std::string& outputHint);
    bool removeOutput(const std::string& outputName);
    std::vector<std::string> outputs() const;

    // Explicit start/stop for all outputs.  Normally you only need stop().
    bool start();
    void stop();
    bool running() const;

    // Connect a source to an output.  The returned RouteId is stable and
    // identifies this specific source->output connection.
    std::optional<RouteId> addRoute(const std::string& source,
                                    const std::string& output,
                                    const MixerStripConfig& cfg);
    bool removeRoute(RouteId id);
    bool renameRoute(RouteId id, const std::string& name);
    bool setRouteVolume(RouteId id, float vol);
    bool setRouteMuted(RouteId id, bool muted);
    bool setRouteKnobIndex(RouteId id, std::optional<int> knobIndex);

    float outputMasterVolume(const std::string& output) const;
    float outputMasterPeak(const std::string& output) const;
    float outputMasterRms(const std::string& output) const;
    void  setOutputMasterVolume(const std::string& output, float v);

    size_t routeCount() const noexcept;
    std::vector<StripSnapshot>   snapshot() const;
    std::optional<StripSnapshot> routeSnapshot(RouteId id) const;

    std::vector<EndpointInfo> listEndpoints() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace anniaudio::core
