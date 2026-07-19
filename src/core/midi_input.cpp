#include "midi_input.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <mmsystem.h>

namespace anniaudio::midi {

class MidiInput::Impl {
public:
    HMIDIIN handle = nullptr;
    Callback callback;

    static void CALLBACK midiInProc(HMIDIIN hMidiIn, UINT wMsg,
                                    DWORD_PTR dwInstance,
                                    DWORD_PTR dwParam1, DWORD_PTR dwParam2)
    {
        (void)hMidiIn;
        (void)dwParam2;
        auto* self = reinterpret_cast<Impl*>(dwInstance);
        if (!self || !self->callback) return;

        if (wMsg == MIM_DATA) {
            MidiMessage msg;
            msg.status = static_cast<uint8_t>(dwParam1 & 0xFF);
            msg.data1  = static_cast<uint8_t>((dwParam1 >> 8) & 0xFF);
            msg.data2  = static_cast<uint8_t>((dwParam1 >> 16) & 0xFF);
            self->callback(msg);
        }
    }
};

namespace {

static bool nameContains(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
        [](char a, char b){ return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
    return it != hay.end();
}

static std::string wideToUtf8(const wchar_t* w)
{
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

} // anonymous namespace

MidiInput::MidiInput() : m_impl(std::make_unique<Impl>()) {}
MidiInput::~MidiInput() { close(); }

std::vector<std::string> MidiInput::listDevices()
{
    std::vector<std::string> out;
    UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps;
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            out.push_back(wideToUtf8(caps.szPname));
        }
    }
    return out;
}

bool MidiInput::open(const std::string& nameHint, Callback cb)
{
    close();
    m_impl->callback = std::move(cb);

    UINT n = midiInGetNumDevs();
    int deviceId = -1;
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps;
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            std::string name = wideToUtf8(caps.szPname);
            std::fprintf(stderr, "[midi] Found input: %s\n", name.c_str());
            if (deviceId < 0 && nameContains(name, nameHint)) {
                deviceId = static_cast<int>(i);
            }
        }
    }

    if (deviceId < 0) {
        std::fprintf(stderr, "[midi] No MIDI input device matched '%s'\n", nameHint.c_str());
        return false;
    }

    MMRESULT r = midiInOpen(&m_impl->handle, static_cast<UINT>(deviceId),
                            reinterpret_cast<DWORD_PTR>(&Impl::midiInProc),
                            reinterpret_cast<DWORD_PTR>(m_impl.get()),
                            CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) {
        std::fprintf(stderr, "[midi] midiInOpen failed: %u\n", static_cast<unsigned>(r));
        return false;
    }

    r = midiInStart(m_impl->handle);
    if (r != MMSYSERR_NOERROR) {
        std::fprintf(stderr, "[midi] midiInStart failed: %u\n", static_cast<unsigned>(r));
        midiInClose(m_impl->handle);
        m_impl->handle = nullptr;
        return false;
    }

    std::fprintf(stderr, "[midi] Opened device %d\n", deviceId);
    return true;
}

void MidiInput::close()
{
    if (m_impl->handle) {
        midiInStop(m_impl->handle);
        midiInClose(m_impl->handle);
        m_impl->handle = nullptr;
    }
    m_impl->callback = nullptr;
}

bool MidiInput::isOpen() const noexcept
{
    return m_impl->handle != nullptr;
}

} // namespace anniaudio::midi
