#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "GroupBus.hpp"
#include "audio_utils.hpp"

namespace anniaudio::core {

// WASAPI render endpoint. Sums the output of one or more GroupBuses, applies
// master volume/mute, and writes to the audio device.
class OutputMixer {
public:
    OutputMixer();
    ~OutputMixer();

    // Open the render endpoint. Must not be called while running.
    bool init(const std::string& outputHint);

    // Start the render thread.
    bool start();

    // Stop and release resources.
    void stop();

    bool running() const;

    // Group buses feeding this output. Can be changed while running; the list is
    // snapshotted under a short lock on each render pass.
    void addGroupBus(GroupBus& bus);
    void removeGroupBus(GroupBus& bus);
    void clearGroupBuses();

    void setMasterVolume(float v); // linear
    void setMasterMuted(bool muted);
    float masterVolume() const;
    bool masterMuted() const;

    const std::string& outputName() const;

private:
    class Impl;
    std::unique_ptr<Impl> p;
};

} // namespace anniaudio::core
