#pragma once

#include "AudioMixer.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace anniaudio::core {

using InputId = uint32_t;
using GroupId = uint32_t;

// An audio source that feeds into one or more groups.
// type = "device" uses a WASAPI endpoint name as source (render loopback or capture).
// type = "application" reserves a process identifier; process-specific loopback
// capture is not yet implemented, so these inputs only produce audio if their
// source also matches a valid endpoint name.
struct InputConfig {
    std::string name;
    std::string type = "device";
    std::string source;
};

// A mix bus (route / group). It has one fader/mute, one colour, a list of
// inputs, and a list of output names it is connected to. All inputs in the
// group are summed and controlled by the group fader.
struct GroupConfig {
    std::string name;
    std::string color = "#3b82f6";
    std::vector<InputId> inputIds;
    std::vector<std::string> outputIds;
    float volume = 1.0f;
    bool muted = false;
    std::optional<int> knobIndex;
};

struct InputSnapshot {
    InputId id = 0;
    std::string name;
    std::string type;
    std::string source;
    float peak = 0.0f;
    float rms = 0.0f;
};

struct GroupSnapshot {
    GroupId id = 0;
    std::string name;
    std::string color;
    std::vector<InputId> inputIds;
    std::vector<std::string> outputIds;
    float volume = 100.0f;
    bool muted = false;
    std::optional<int> knobIndex;
    float peak = 0.0f;
    float rms = 0.0f;
};

struct OutputSnapshot {
    std::string name;
    float master = 100.0f;
    std::vector<GroupId> groupIds;
    float masterPeak = 0.0f;
    float masterRms = 0.0f;
};

struct MixerStateSnapshot {
    bool running = false;
    uint16_t controlPort = 8850;
    std::vector<InputSnapshot> inputs;
    std::vector<GroupSnapshot> groups;
    std::vector<OutputSnapshot> outputs;
};

// Mixer routing matrix: inputs -> groups -> outputs.
//
// For Stage 1 each (input, output) pair that a group touches is still opened
// as a separate strip on the per-output AudioMixer. This means sending a
// microphone group to two outputs opens the microphone twice. A future shared
// group bus will collapse that, but the model, API, and TUI already treat
// groups as the single entity.
class AudioMixerMatrix {
public:
    using RouteId = uint64_t; // internal only; not exposed through the API

    AudioMixerMatrix();
    ~AudioMixerMatrix();

    AudioMixerMatrix(const AudioMixerMatrix&) = delete;
    AudioMixerMatrix& operator=(const AudioMixerMatrix&) = delete;

    // -----------------------------------------------------------------------
    // Outputs
    // -----------------------------------------------------------------------
    bool addOutput(const std::string& outputHint);
    bool removeOutput(const std::string& outputName);
    std::vector<std::string> outputNames() const;

    float outputMasterVolume(const std::string& outputName) const;
    float outputMasterPeak(const std::string& outputName) const;
    float outputMasterRms(const std::string& outputName) const;
    void  setOutputMasterVolume(const std::string& outputName, float v);

    // -----------------------------------------------------------------------
    // Inputs
    // -----------------------------------------------------------------------
    std::optional<InputId> addInput(const InputConfig& cfg);
    bool removeInput(InputId id);
    bool updateInput(InputId id, const InputConfig& cfg); // re-creates routes for groups using it

    // -----------------------------------------------------------------------
    // Groups
    // -----------------------------------------------------------------------
    std::optional<GroupId> addGroup(const GroupConfig& cfg);
    bool removeGroup(GroupId id);

    bool setGroupName(GroupId id, const std::string& name);
    bool setGroupColor(GroupId id, const std::string& color);
    bool setGroupVolume(GroupId id, float vol);
    bool setGroupMuted(GroupId id, bool muted);
    bool setGroupKnobIndex(GroupId id, std::optional<int> knobIndex);
    bool setGroupInputIds(GroupId id, std::vector<InputId> ids);
    bool setGroupOutputIds(GroupId id, std::vector<std::string> ids);

    bool addGroupInput(GroupId groupId, InputId inputId);
    bool removeGroupInput(GroupId groupId, InputId inputId);
    bool addGroupOutput(GroupId groupId, const std::string& outputName);
    bool removeGroupOutput(GroupId groupId, const std::string& outputName);

    // -----------------------------------------------------------------------
    // Transport / info
    // -----------------------------------------------------------------------
    bool start();
    void stop();
    bool running() const;

    MixerStateSnapshot snapshot() const;
    std::vector<EndpointInfo> listEndpoints() const;

    // -----------------------------------------------------------------------
    // Preset persistence & autosave
    // -----------------------------------------------------------------------
    bool save(const std::string& path) const;
    bool load(const std::string& path);
    void setAutosavePath(const std::string& path);
    void setControlPort(uint16_t port);
    bool autosaveEnabled() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    mutable std::string m_autosavePath;
    mutable uint16_t m_controlPort = 8850;

    MixerStateSnapshot snapshotNoLock() const;
    bool saveSnapshot(const std::string& path, const MixerStateSnapshot& s) const;
    void maybeAutosave() const;
    bool rebuildGroupRoutesLocked(GroupId id);
    bool addOutputLocked(const std::string& outputHint);
};

} // namespace anniaudio::core
