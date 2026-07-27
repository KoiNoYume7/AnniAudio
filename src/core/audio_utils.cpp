#include "audio_utils.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include <audiopolicy.h>
#include <audioclientactivationparams.h>
#include <objidl.h>
#include <tlhelp32.h>

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")

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

void EnsureComInitializedOnThisThread()
{
    thread_local bool initialized = false;
    if (initialized) return;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    initialized = true;
}

namespace {

class ProcessLoopbackActivationHandler :
    public IActivateAudioInterfaceCompletionHandler,
    public IAgileObject {
public:
    ProcessLoopbackActivationHandler() : m_ref(1), m_event(CreateEvent(nullptr, FALSE, FALSE, nullptr)) {}
    ~ProcessLoopbackActivationHandler() { if (m_event) CloseHandle(m_event); }

    HANDLE eventHandle() const { return m_event; }
    HRESULT result() const { return m_result; }
    IAudioClient* client() const { return m_client.Get(); }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, __uuidof(IActivateAudioInterfaceCompletionHandler))) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (IsEqualIID(riid, IID_IAgileObject)) {
            *ppv = static_cast<IAgileObject*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&m_ref); }
    STDMETHOD_(ULONG, Release)() override {
        ULONG c = InterlockedDecrement(&m_ref);
        if (c == 0) delete this;
        return c;
    }

    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override {
        m_result = E_FAIL;
        if (!operation) {
            SetEvent(m_event);
            return S_OK;
        }
        HRESULT hrActivate = E_UNEXPECTED;
        ComPtr<IUnknown> punk;
        HRESULT hr = operation->GetActivateResult(&hrActivate, &punk);
        if (SUCCEEDED(hr) && SUCCEEDED(hrActivate) && punk) {
            punk.As(&m_client);
            m_result = m_client ? S_OK : E_NOINTERFACE;
        } else {
            m_result = FAILED(hrActivate) ? hrActivate : hr;
        }
        SetEvent(m_event);
        return S_OK;
    }

private:
    volatile LONG m_ref;
    HANDLE m_event;
    HRESULT m_result = E_FAIL;
    ComPtr<IAudioClient> m_client;
};

} // namespace

ComPtr<IAudioClient> activateProcessLoopbackClient(uint32_t pid, DWORD timeoutMs)
{
    EnsureComInitializedOnThisThread();

    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    params.ProcessLoopbackParams.TargetProcessId = pid;

    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    ComPtr<ProcessLoopbackActivationHandler> handler;
    handler.Attach(new ProcessLoopbackActivationHandler());

    ComPtr<IActivateAudioInterfaceAsyncOperation> asyncOp;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &pv,
        handler.Get(),
        &asyncOp);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[audio_utils] ActivateAudioInterfaceAsync failed 0x%08X\n", (unsigned)hr);
        return nullptr;
    }

    DWORD wait = WaitForSingleObject(handler->eventHandle(), timeoutMs);
    if (wait != WAIT_OBJECT_0) {
        std::fprintf(stderr, "[audio_utils] Timeout waiting for process loopback activation (pid %u)\n", pid);
        return nullptr;
    }

    hr = handler->result();
    if (FAILED(hr) || !handler->client()) {
        std::fprintf(stderr, "[audio_utils] Process loopback activation failed for pid %u: 0x%08X\n", pid, (unsigned)hr);
        return nullptr;
    }

    ComPtr<IAudioClient> client;
    client.Attach(handler->client());
    client->AddRef();
    return client;
}

bool initProcessLoopbackFormat(WAVEFORMATEXTENSIBLE& wfex)
{
    wfex.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wfex.Format.nChannels       = 2;
    wfex.Format.nSamplesPerSec  = 48000;
    wfex.Format.wBitsPerSample  = 32;
    wfex.Format.nBlockAlign     = wfex.Format.nChannels * sizeof(float);
    wfex.Format.nAvgBytesPerSec = wfex.Format.nSamplesPerSec * wfex.Format.nBlockAlign;
    wfex.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfex.Samples.wValidBitsPerSample = 32;
    wfex.dwChannelMask          = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wfex.SubFormat              = KSCONST_SUBTYPE_IEEE_FLOAT;
    return true;
}

std::string resolveHrtfPath()
{
    auto exists = [](const std::string& p) {
        DWORD a = GetFileAttributesA(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    };
    const char* rel = "assets/hrtf/mit_kemar.sofa";
    if (exists(rel)) return rel;

    char exePath[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
        std::string dir(exePath);
        size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) dir.resize(slash);
        for (const char* up : {"/assets/hrtf/mit_kemar.sofa",
                               "/../../../assets/hrtf/mit_kemar.sofa"}) {
            std::string cand = dir + up;
            if (exists(cand)) return cand;
        }
    }
    return rel;
}

std::vector<EndpointInfo> enumEndpoints(IMMDeviceEnumerator* enumerator)
{
    std::vector<EndpointInfo> out;
    if (!enumerator) return out;

    // Determine defaults
    ComPtr<IMMDevice> defRender, defCapture;
    enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defRender);
    enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &defCapture);

    wchar_t* defRenderId = nullptr; wchar_t* defCaptureId = nullptr;
    if (defRender) defRender->GetId(&defRenderId);
    if (defCapture) defCapture->GetId(&defCaptureId);
    std::string defRenderStr = defRenderId ? wideToUtf8(defRenderId) : "";
    std::string defCaptureStr = defCaptureId ? wideToUtf8(defCaptureId) : "";
    if (defRenderId) CoTaskMemFree(defRenderId);
    if (defCaptureId) CoTaskMemFree(defCaptureId);

    for (int pass = 0; pass < 2; ++pass) {
        EDataFlow flow = (pass == 0) ? eRender : eCapture;
        ComPtr<IMMDeviceCollection> col;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) continue;
        UINT count = 0; col->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            if (FAILED(col->Item(i, &dev))) continue;
            wchar_t* id = nullptr; dev->GetId(&id);
            std::string idStr = id ? wideToUtf8(id) : "";
            if (id) CoTaskMemFree(id);
            std::string name = friendlyName(dev.Get());
            bool isDefault = (flow == eRender)
                ? (idStr == defRenderStr)
                : (idStr == defCaptureStr);
            out.push_back({ idStr, name, flow == eRender,
                            nameContains(name, "AnniAudio"), isDefault });
        }
    }
    return out;
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

static std::string processNameFromId(DWORD pid)
{
    if (pid == 0) return "System Sounds";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    char path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameA(h, 0, path, &size);
    CloseHandle(h);
    if (!ok) return {};
    if (const char* base = strrchr(path, '\\')) return std::string(base + 1);
    if (const char* base = strrchr(path, '/')) return std::string(base + 1);
    return std::string(path);
}

static std::string exeNameFromSessionIdentifier(IAudioSessionControl2* ctrl2)
{
    // Fallback for processes OpenProcess cannot touch (elevated apps, games
    // with anticheat): the session identifier embeds the executable path, e.g.
    // "{guid}|\Device\HarddiskVolume3\...\Spotify.exe%b{guid}".
    wchar_t* sid = nullptr;
    if (FAILED(ctrl2->GetSessionIdentifier(&sid)) || !sid) return {};
    std::string s = wideToUtf8(sid);
    CoTaskMemFree(sid);
    size_t exe = s.find(".exe");
    if (exe == std::string::npos) return {};
    size_t start = s.find_last_of("\\/", exe);
    if (start == std::string::npos) return {};
    return s.substr(start + 1, exe + 4 - (start + 1));
}

static BOOL CALLBACK collectWindowTitle(HWND hwnd, LPARAM lp)
{
    auto* titles = reinterpret_cast<std::unordered_map<DWORD, std::string>*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE; // skip owned popups
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || titles->count(pid)) return TRUE; // keep the first (topmost in z-order)
    wchar_t buf[256] = {};
    if (GetWindowTextW(hwnd, buf, 256) <= 0) return TRUE;
    (*titles)[pid] = wideToUtf8(buf);
    return TRUE;
}

// pid -> (parent pid, exe name) for every running process. Used to find a
// window title for processes that render audio in a windowless child (e.g.
// browser audio utility processes).
struct ProcParent { DWORD ppid = 0; std::string exe; };
static std::unordered_map<DWORD, ProcParent> processParentTable()
{
    std::unordered_map<DWORD, ProcParent> table;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return table;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcParent pp;
            pp.ppid = pe.th32ParentProcessID;
            pp.exe = wideToUtf8(pe.szExeFile);
            table[pe.th32ProcessID] = std::move(pp);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return table;
}

std::vector<ApplicationInfo> enumAudioSessions(IMMDeviceEnumerator* enumerator)
{
    std::vector<ApplicationInfo> out;
    if (!enumerator) return out;

    std::unordered_map<DWORD, ApplicationInfo> best;
    std::unordered_map<DWORD, int> bestScore;

    EDataFlow flows[2] = { eRender, eCapture };
    for (EDataFlow flow : flows) {
        ComPtr<IMMDeviceCollection> col;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) continue;
        UINT devCount = 0; col->GetCount(&devCount);
        for (UINT d = 0; d < devCount; ++d) {
            ComPtr<IMMDevice> dev;
            if (FAILED(col->Item(d, &dev))) continue;
            std::string endpointName = friendlyName(dev.Get());

            ComPtr<IAudioSessionManager2> mgr;
            if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(mgr.GetAddressOf()))))
                continue;

            ComPtr<IAudioSessionEnumerator> sessions;
            if (FAILED(mgr->GetSessionEnumerator(&sessions))) continue;

            int sessionCount = 0;
            sessions->GetCount(&sessionCount);
            for (int s = 0; s < sessionCount; ++s) {
                ComPtr<IAudioSessionControl> ctrl;
                if (FAILED(sessions->GetSession(s, &ctrl))) continue;

                ComPtr<IAudioSessionControl2> ctrl2;
                if (FAILED(ctrl.As(&ctrl2))) continue;

                DWORD pid = 0;
                if (FAILED(ctrl2->GetProcessId(&pid))) continue;

                // Skip sessions whose process has already exited; they linger
                // in the enumerator until the session manager expires them.
                AudioSessionState sessionState = AudioSessionStateInactive;
                if (SUCCEEDED(ctrl->GetState(&sessionState)) && sessionState == AudioSessionStateExpired)
                    continue;

                // One entry per process id. Apps can hold sessions on several
                // endpoints at once (e.g. before and after being re-routed);
                // report the endpoint of the session that is actually playing,
                // preferring active over inactive, then render over capture.
                bool active = (sessionState == AudioSessionStateActive);
                int score = (active ? 2 : 0) + (flow == eRender ? 1 : 0);
                auto existing = best.find(pid);
                if (existing != best.end() && bestScore[pid] >= score) continue;

                ComPtr<ISimpleAudioVolume> vol;
                BOOL muted = FALSE;
                float volume = 1.0f;
                if (SUCCEEDED(ctrl.As(&vol))) {
                    vol->GetMute(&muted);
                    vol->GetMasterVolume(&volume);
                }

                wchar_t* disp = nullptr;
                std::string displayName;
                if (SUCCEEDED(ctrl->GetDisplayName(&disp)) && disp) {
                    displayName = wideToUtf8(disp);
                    CoTaskMemFree(disp);
                }

                std::string name = processNameFromId(pid);
                if (name.empty()) name = exeNameFromSessionIdentifier(ctrl2.Get());
                if (name.empty()) {
                    name = displayName.empty() ? (flow == eRender ? "Unknown app" : "Unknown capture")
                                               : displayName;
                }

                ApplicationInfo info;
                info.processId = pid;
                info.name = name;
                info.displayName = displayName;
                info.endpoint = endpointName;
                info.isInput = (flow == eCapture);
                info.isActive = active;
                info.isMuted = (muted != FALSE);
                info.volume = volume;
                info.isSystem = (pid == 0);

                best[pid] = std::move(info);
                bestScore[pid] = score;
            }
        }
    }

    // Attach a window title as an identification helper: exe names often have
    // nothing to do with what the user sees on screen. If the audio process is
    // windowless, walk up the parent chain -- but only through processes with
    // the same exe name, so a browser's audio child finds the browser window
    // without ever walking up into explorer.exe.
    std::unordered_map<DWORD, std::string> titles;
    EnumWindows(collectWindowTitle, reinterpret_cast<LPARAM>(&titles));
    auto parents = processParentTable();
    for (auto& kv : best) {
        ApplicationInfo& info = kv.second;
        DWORD cur = kv.first;
        for (int hop = 0; hop < 4; ++hop) {
            auto t = titles.find(cur);
            if (t != titles.end()) { info.windowTitle = t->second; break; }
            auto p = parents.find(cur);
            if (p == parents.end()) break;
            auto pp = parents.find(p->second.ppid);
            if (pp == parents.end()) break;
            if (_stricmp(pp->second.exe.c_str(), info.name.c_str()) != 0) break;
            cur = p->second.ppid;
        }
    }

    out.reserve(best.size());
    for (auto& kv : best) out.push_back(std::move(kv.second));
    std::sort(out.begin(), out.end(), [](const ApplicationInfo& a, const ApplicationInfo& b) {
        if (a.isSystem != b.isSystem) return a.isSystem < b.isSystem;
        return a.name < b.name;
    });
    return out;
}

} // namespace anniaudio::core
