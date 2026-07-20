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
// type = "application" uses the source as a decimal process ID and captures that
// process's audio via Windows process loopback capture.
struct InputConfig {
    std::string name;
    std::string type = "device";
    // Capture-side DSP applied wherever this input is routed (see
    // MixerStripConfig): RNNoise suppression and/or a named EQ preset.
    bool denoise = false;
    std::string eqPreset; // "" = off; "voice"
    std::string source;
};

// A mix bus (route / group). It has one fader/mute, one colour, a list of
// inputs, and a list of output names it is connected to. All inputs in the
// group are summed and controlled by the group fader.
//
// `cable` is an optional render endpoint name (e.g. "Music (Virtual Audio Cable)").
// When an application input is added to a group that has a cable, the mixer
// routes that application's per-app default output to the cable. The mixer then
// captures the cable as a device input, avoiding double audio. If cable is
// empty, application inputs fall back to process loopback capture.
struct GroupConfig {
    std::string name;
    std::string color = "#3b82f6";
    std::string cable; // optional VAC / render endpoint for app routing
    std::vector<InputId> inputIds;
    std::vector<std::string> outputIds;
    // Per-output send gain (output name -> linear gain, 1.0 = unity). The
    // effective strip volume for a route is group volume * send gain, so one
    // group can e.g. run at 100% into headphones but 15% into speakers.
    // Outputs without an entry are at unity.
    std::map<std::string, float> outputGains;
    float volume = 1.0f;
    bool muted = false;
    std::optional<int> knobIndex;
};

struct InputSnapshot {
    InputId id = 0;
    std::string name;
    std::string type;
    std::string source;
    bool denoise = false;
    std::string eqPreset;
    float peak = 0.0f;
    float rms = 0.0f;
};

struct GroupSnapshot {
    GroupId id = 0;
    std::string name;
    std::string color;
    std::string cable;
    std::vector<InputId> inputIds;
    std::vector<std::string> outputIds;
    std::map<std::string, float> outputGains; // output name -> send gain in percent
    float volume = 100.0f;
    bool muted = false;
    std::optional<int> knobIndex;
    float peak = 0.0f;
    float rms = 0.0f;
};

struct OutputSnapshot {
    std::string name;
    float master = 100.0f;
    bool muted = false;
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
    bool setGroupCable(GroupId id, const std::string& cable);
    bool setGroupVolume(GroupId id, float vol);
    bool setGroupMuted(GroupId id, bool muted);
    // Send gain (percent, 100 = unity) for one group -> output route.
    bool setGroupOutputGain(GroupId id, const std::string& output, float gainPct);
    bool setOutputMuted(const std::string& outputName, bool muted);
    bool outputMuted(const std::string& outputName) const;
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
    std::vector<ApplicationInfo> listApplications() const;

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
