#include "InputProcessor.hpp"
#include "audio_utils.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace anniaudio::core;

int main(int argc, char** argv)
{
    EnsureComInitializedOnThisThread();

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) {
        std::fprintf(stderr, "CoCreateInstance(MMDeviceEnumerator) failed 0x%08X\n", (unsigned)hr);
        return 1;
    }

    ComPtr<IMMDevice> dev;
    hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
    if (FAILED(hr) || !dev) {
        std::fprintf(stderr, "No default capture device (0x%08X). Falling back to first render loopback.\n", (unsigned)hr);
        ComPtr<IMMDeviceCollection> col;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
            UINT count = 0;
            col->GetCount(&count);
            if (count > 0) col->Item(0, &dev);
        }
        if (!dev) {
            std::fprintf(stderr, "No usable audio device found.\n");
            return 1;
        }
    }

    std::string name = friendlyName(dev.Get());
    std::fprintf(stderr, "[test_input_processor] Using device: %s\n", name.c_str());

    InputProcessorConfig cfg;
    cfg.name   = "test";
    cfg.type   = "device";
    cfg.source = name;
    cfg.spatial = (argc > 1 && std::string(argv[1]) == "--spatial");
    cfg.denoise = (argc > 1 && std::string(argv[1]) == "--denoise");
    if (argc > 1 && std::string(argv[1]) == "--voice") cfg.eqPreset = "voice";

    InputProcessor proc;
    if (!proc.init(cfg)) {
        std::fprintf(stderr, "[test_input_processor] init failed\n");
        return 1;
    }
    if (!proc.start()) {
        std::fprintf(stderr, "[test_input_processor] start failed\n");
        return 1;
    }

    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::fprintf(stderr, "[test_input_processor] ring available: %zu samples\n",
                     proc.outputRing().available());
    }

    proc.stop();
    std::fprintf(stderr, "[test_input_processor] stopped\n");
    return 0;
}
