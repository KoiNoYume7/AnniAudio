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

// Format conversion: src (float) -> dst (float), handling channel count
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

} // namespace anniaudio::core
