#include "audio_utils.hpp"

#include <cstdio>
#include <cstring>

namespace anniaudio::core {

const GUID KSCONST_SUBTYPE_IEEE_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
const GUID KSCONST_SUBTYPE_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

std::string wideToUtf8(const wchar_t* w)
{
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string friendlyName(IMMDevice* dev)
{
    ComPtr<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return {};
    PROPVARIANT pv; PropVariantInit(&pv);
    props->GetValue(PKEY_Device_FriendlyName, &pv);
    std::string name = (pv.vt == VT_LPWSTR) ? wideToUtf8(pv.pwszVal) : std::string{};
    PropVariantClear(&pv);
    return name;
}

bool nameContains(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
        [](char a, char b){ return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); })
        != hay.end();
}

bool isFloatWfx(const WAVEFORMATEX* wfx)
{
    if (!wfx) return false;
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        auto wfex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        return IsEqualGUID(wfex->SubFormat, KSCONST_SUBTYPE_IEEE_FLOAT);
    }
    return false;
}

bool isPcm16Wfx(const WAVEFORMATEX* wfx)
{
    if (!wfx) return false;
    if (wfx->wFormatTag == WAVE_FORMAT_PCM) return wfx->wBitsPerSample == 16;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        auto wfex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        return IsEqualGUID(wfex->SubFormat, KSCONST_SUBTYPE_PCM) && wfex->Format.wBitsPerSample == 16;
    }
    return false;
}

WfxPtr makeFloatFormat(const WAVEFORMATEX* mix)
{
    if (!mix) return nullptr;
    auto* wfex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    if (!wfex) return nullptr;

    WORD ch = mix->nChannels;
    DWORD rate = mix->nSamplesPerSec;
    WORD bytes = ch * sizeof(float);

    wfex->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wfex->Format.nChannels       = ch;
    wfex->Format.nSamplesPerSec  = rate;
    wfex->Format.nAvgBytesPerSec = rate * bytes;
    wfex->Format.nBlockAlign     = bytes;
    wfex->Format.wBitsPerSample  = 32;
    wfex->Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfex->Samples.wValidBitsPerSample = 32;
    wfex->dwChannelMask          = (ch == 1) ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    wfex->SubFormat              = KSCONST_SUBTYPE_IEEE_FLOAT;

    return WfxPtr(reinterpret_cast<WAVEFORMATEX*>(wfex));
}

WfxPtr tryForceFloat(IAudioClient* client, const WAVEFORMATEX* mix)
{
    WfxPtr floatFmt = makeFloatFormat(mix);
    if (!floatFmt || !client) return nullptr;

    WAVEFORMATEX* closest = nullptr;
    HRESULT hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, floatFmt.get(), &closest);

    if (hr == S_OK) {
        return floatFmt;
    }
    if (hr == S_FALSE && closest && isFloatWfx(closest)) {
        return WfxPtr(closest);
    }
    if (closest) CoTaskMemFree(closest);
    return nullptr;
}

ComPtr<IMMDevice> findDevice(IMMDeviceEnumerator* enumerator,
                              EDataFlow flow, const std::string& hint)
{
    ComPtr<IMMDevice> result;
    if (hint.empty()) { enumerator->GetDefaultAudioEndpoint(flow, eConsole, &result); return result; }
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return result;
    UINT count = 0; col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (SUCCEEDED(col->Item(i, &dev)) && nameContains(friendlyName(dev.Get()), hint)) {
            result = dev; break;
        }
    }
    return result;
}

ComPtr<IMMDevice> findAnyDevice(IMMDeviceEnumerator* enumerator,
                                 const std::string& hint,
                                 EDataFlow* foundFlow)
{
    ComPtr<IMMDevice> renderDev;
    ComPtr<IMMDevice> captureDev;
    EDataFlow flows[2] = { eRender, eCapture };
    for (EDataFlow flow : flows) {
        ComPtr<IMMDeviceCollection> col;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) continue;
        UINT count = 0; col->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            if (SUCCEEDED(col->Item(i, &dev)) && nameContains(friendlyName(dev.Get()), hint)) {
                if (flow == eRender) renderDev = dev;
                else captureDev = dev;
                break;
            }
        }
    }
    if (captureDev && renderDev) {
        std::fprintf(stderr, "[Engine] Warning: name '%s' matches both render and capture endpoints. Using capture.\n", hint.c_str());
        *foundFlow = eCapture;
        return captureDev;
    }
    if (captureDev) { *foundFlow = eCapture; return captureDev; }
    if (renderDev) { *foundFlow = eRender; return renderDev; }
    *foundFlow = eAll;
    return nullptr;
}

uint32_t convertBuffer(
    const float* src, uint32_t srcFrames, uint32_t srcCh,
    float*       dst, uint32_t dstMaxFrames, uint32_t dstCh,
    double srcToDstRatio,
    double& phase)
{
    uint32_t dstFrame = 0;
    while (dstFrame < dstMaxFrames) {
        auto   idx0  = static_cast<uint32_t>(phase);
        double frac  = phase - idx0;
        uint32_t idx1 = std::min(idx0 + 1, srcFrames - 1);

        if (idx0 >= srcFrames) break;

        uint32_t chsToCopy = std::min(srcCh, dstCh);
        for (uint32_t c = 0; c < chsToCopy; ++c) {
            float s0 = src[idx0 * srcCh + c];
            float s1 = src[idx1 * srcCh + c];
            dst[dstFrame * dstCh + c] = s0 + static_cast<float>(frac) * (s1 - s0);
        }
        for (uint32_t c = chsToCopy; c < dstCh; ++c)
            dst[dstFrame * dstCh + c] = 0.0f;

        phase += 1.0 / srcToDstRatio;
        ++dstFrame;
    }
    phase = std::max(0.0, phase - srcFrames);
    return dstFrame;
}

void pcm16ToFloat(const BYTE* src, size_t sampleCount, float* dst)
{
    auto* p = reinterpret_cast<const int16_t*>(src);
    const float scale = 1.0f / 32768.0f;
    for (size_t i = 0; i < sampleCount; ++i) dst[i] = p[i] * scale;
}

void floatToPcm16(const float* src, size_t sampleCount, BYTE* dst)
{
    auto* p = reinterpret_cast<int16_t*>(dst);
    for (size_t i = 0; i < sampleCount; ++i) {
        float s = src[i];
        if (s > 1.0f) s = 1.0f;
        else if (s < -1.0f) s = -1.0f;
        p[i] = static_cast<int16_t>(s * 32767.0f);
    }
}

} // namespace anniaudio::core
