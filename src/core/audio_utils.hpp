#pragma once

#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace anniaudio::core {

using Microsoft::WRL::ComPtr;

struct EndpointInfo {
    std::string id;          // WASAPI device ID (e.g. \\{...}.#{...})
    std::string name;        // Friendly name (e.g. "AnniAudio Cable 1")
    bool        isRender;    // true = render (playback), false = capture (recording)
    bool        isAnniAudio; // true if name contains "AnniAudio" or matches a cable config name
    bool        isDefault;   // true if this is the Windows default for its direction
};

// A running audio application session. Process loopback capture can target
// the process ID; render/capture sessions for the same process are deduplicated.
struct ApplicationInfo {
    uint32_t    processId = 0;
    std::string name;        // executable name (e.g. "chrome.exe")
    std::string displayName; // session display name (may be empty)
    std::string windowTitle; // main window title, as an identification helper (may be empty)
    std::string endpoint;    // friendly name of the device the session lives on
    bool        isInput = false; // true = capture, false = render
    bool        isActive = false; // true if the reported session is currently playing
    bool        isMuted = false;
    float       volume = 1.0f;
    bool        isSystem = false; // true if process id is 0 (system sounds session)
};

// Enumerate all active endpoints (render + capture) into EndpointInfo structs.
// Caller must have called CoInitializeEx on the current thread.
std::vector<EndpointInfo> enumEndpoints(IMMDeviceEnumerator* enumerator);

// Enumerate active audio sessions across all active endpoints and return one
// ApplicationInfo per distinct process ID. Caller must have COM initialized.
std::vector<ApplicationInfo> enumAudioSessions(IMMDeviceEnumerator* enumerator);

// Local definitions for KS audio format subtypes; avoids linking against
// ksuser/ksguid for the standard KSDATAFORMAT_SUBTYPE_* GUIDs.
extern const GUID KSCONST_SUBTYPE_IEEE_FLOAT;
extern const GUID KSCONST_SUBTYPE_PCM;

struct WfxDeleter { void operator()(WAVEFORMATEX* p) { CoTaskMemFree(p); } };
using WfxPtr = std::unique_ptr<WAVEFORMATEX, WfxDeleter>;

std::string wideToUtf8(const wchar_t* w);
std::wstring utf8ToWide(const std::string& s);
std::string friendlyName(IMMDevice* dev);
bool nameContains(const std::string& hay, const std::string& needle);

bool isFloatWfx(const WAVEFORMATEX* wfx);
bool isPcm16Wfx(const WAVEFORMATEX* wfx);
WfxPtr makeFloatFormat(const WAVEFORMATEX* mix);
WfxPtr tryForceFloat(IAudioClient* client, const WAVEFORMATEX* mix);

ComPtr<IMMDevice> findDevice(IMMDeviceEnumerator* enumerator,
                              EDataFlow flow, const std::string& hint);
ComPtr<IMMDevice> findAnyDevice(IMMDeviceEnumerator* enumerator,
                                 const std::string& hint,
                                 EDataFlow* foundFlow);

// Initialize COM as multi-threaded on the current thread once. Safe to call
// from any worker thread; does nothing after the first call.
void EnsureComInitializedOnThisThread();

// Activates an IAudioClient for process-loopback capture of `pid`.
// Waits up to `timeoutMs` for the asynchronous activation to complete.
ComPtr<IAudioClient> activateProcessLoopbackClient(uint32_t pid, DWORD timeoutMs = 10000);

// Fill `wfex` with the float 48 kHz stereo format used for process loopback.
bool initProcessLoopbackFormat(WAVEFORMATEXTENSIBLE& wfex);

// Locate a SOFA HRTF dataset. If userPath is non-empty and the file exists, it
// is used; otherwise falls back to the bundled MIT KEMAR SOFA. The fallback
// searches from the repo root to paths relative to the executable so tests and
// installed builds both work.
std::string resolveHrtfPath(const std::string& userPath = std::string{});

// Format conversion: src (float) -> dst (float), handling sample-rate
// mismatch and sample-rate mismatch via linear interpolation.
// phase persists between calls.
uint32_t convertBuffer(
    const float* src, uint32_t srcFrames, uint32_t srcCh,
    float*       dst, uint32_t dstMaxFrames, uint32_t dstCh,
    double srcToDstRatio,
    double& phase);

void pcm16ToFloat(const BYTE* src, size_t sampleCount, float* dst);
void floatToPcm16(const float* src, size_t sampleCount, BYTE* dst);

// ---------------------------------------------------------------------------
// Simple single-producer/single-consumer ring buffer (float samples)
// ---------------------------------------------------------------------------
class RingBuffer {
public:
    void init(size_t capacity) {
        m_buf.assign(capacity, 0.0f);
        m_head = m_tail = 0; m_cap = capacity;
    }
    size_t available() const {
        return (m_head >= m_tail) ? m_head - m_tail : m_cap - (m_tail - m_head);
    }
    void write(const float* src, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            m_buf[m_head] = src[i];
            m_head = (m_head + 1) % m_cap;
            if (m_head == m_tail) m_tail = (m_tail + 1) % m_cap; // drop oldest on overflow
        }
    }
    void readOrSilence(float* dst, size_t n) {
        size_t got = std::min(n, available());
        for (size_t i = 0; i < got;  ++i) { dst[i] = m_buf[m_tail]; m_tail = (m_tail + 1) % m_cap; }
        for (size_t i = got; i < n;  ++i) dst[i] = 0.0f;
    }
private:
    std::vector<float> m_buf;
    size_t m_head{0}, m_tail{0}, m_cap{0};
};

// ---------------------------------------------------------------------------
// Multi-reader ring buffer (float samples)
// ---------------------------------------------------------------------------
// One producer writes; each consumer keeps its own absolute read cursor.
// Slow consumers skip ahead when the producer laps the buffer, so fast
// consumers are not held back. All operations are lock-free but not wait-free;
// consumers read from a snapshot of m_head and may see a slightly stale head.
class MultiReaderRingBuffer {
public:
    void init(size_t capacity) {
        m_buf.assign(capacity, 0.0f);
        m_cap = capacity;
        m_head.store(0);
    }

    size_t capacity() const { return m_cap; }

    size_t write(const float* src, size_t n) {
        size_t head = m_head.load();
        for (size_t i = 0; i < n; ++i) {
            m_buf[(head + i) % m_cap] = src[i];
        }
        m_head.store(head + n);
        return n;
    }

    // Absolute write index; consumers can use this to initialize their read
    // cursor at the current head (skipping any data already in the buffer).
    size_t head() const { return m_head.load(); }

    // Number of samples available to read from `tail`.
    size_t available(size_t tail) const {
        return m_head.load() - tail;
    }

    // Read up to `n` samples into `dst`, advancing `tail`. Missing samples are
    // zero-filled. Returns the number of non-silent samples read.
    size_t readOrSilence(float* dst, size_t n, size_t& tail) {
        size_t head = m_head.load();
        if (head - tail > m_cap) {
            // Producer has lapped this consumer; skip the overwritten samples.
            tail = head - m_cap;
        }
        size_t got = std::min(n, head - tail);
        for (size_t i = 0; i < got; ++i) {
            dst[i] = m_buf[(tail + i) % m_cap];
        }
        for (size_t i = got; i < n; ++i) dst[i] = 0.0f;
        tail += got;
        return got;
    }

private:
    std::vector<float> m_buf;
    std::atomic<size_t> m_head{0};
    size_t m_cap{0};
};

} // namespace anniaudio::core
