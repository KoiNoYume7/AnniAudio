#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace anniaudio::midi {

struct MidiMessage {
    uint8_t status = 0;
    uint8_t data1  = 0;
    uint8_t data2  = 0;
};

// Lightweight Windows MIDI input wrapper.
class MidiInput {
public:
    using Callback = std::function<void(const MidiMessage&)>;

    MidiInput();
    ~MidiInput();

    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    // Find an input device whose name contains nameHint and open it.
    bool open(const std::string& nameHint, Callback cb);
    void close();
    bool isOpen() const noexcept;

    static std::vector<std::string> listDevices();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace anniaudio::midi
