// test_routing.cpp
// Routing engine test harness
//
// Usage: test_routing [capture_hint] [render_hint]
//
//   capture_hint : case-insensitive substring of the capture device name
//                  default: "AnniAudio"  (matches the AnniAudio Cable capture endpoint)
//   render_hint  : case-insensitive substring of the render device name
//                  default: ""           (uses Windows default render device)
//
// Examples:
//   test_routing                              -- AnniAudio capture -> default output
//   test_routing "AnniAudio" "Logitech"       -- AnniAudio -> Logitech G560
//   test_routing "AnniAudio" "Headphones"     -- AnniAudio -> headphones
//
// To route audio: set an app's output to the "AnniAudio Cable 1" render endpoint
// in Windows sound settings or the app itself.  The audio flows through the kernel
// driver's shared cyclic buffer and surfaces on the AnniAudio capture endpoint,
// which this engine reads and re-renders to the real output device.
//
// Press Ctrl+C or wait 5 minutes to stop.

#include "AudioEngine.hpp"
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <atomic>
#include <string>

static std::atomic<bool> g_quit{false};

static BOOL WINAPI consoleCtrl(DWORD sig)
{
    if (sig == CTRL_C_EVENT || sig == CTRL_BREAK_EVENT) {
        g_quit = true;
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char* argv[])
{
    std::string captureHint = (argc > 1) ? argv[1] : "AnniAudio";
    std::string renderHint  = (argc > 2) ? argv[2] : "";

    std::printf("AnniAudio — Routing Engine\n");
    std::printf("  Capture : \"%s\"%s\n", captureHint.c_str(), captureHint.empty() ? " (default)" : "");
    std::printf("  Render  : \"%s\"%s\n", renderHint.c_str(),  renderHint.empty()  ? " (default)" : "");
    std::printf("\n");

    SetConsoleCtrlHandler(consoleCtrl, TRUE);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { std::fprintf(stderr, "CoInitializeEx failed\n"); return 1; }

    AudioEngine engine;

    if (!engine.start(captureHint, renderHint)) {
        std::fprintf(stderr, "\nFailed to start engine. Check device names and format settings.\n");
        CoUninitialize();
        return 1;
    }

    std::printf("\nEngine running — %u Hz, %u ch\n", engine.sampleRate(), engine.channelCount());
    std::printf("Route audio to the capture device to hear it on the render device.\n");
    std::printf("Press Ctrl+C to stop.\n\n");

    uint64_t lastFrames = 0;
    for (int sec = 1; sec <= 300 && !g_quit; ++sec) {
        Sleep(1000);
        if (!engine.isRunning()) { std::printf("Engine stopped unexpectedly.\n"); break; }
        uint64_t total = engine.framesProcessed();
        std::printf("[%3ds] %6llu fps   total %llu\n",
                    sec,
                    (unsigned long long)(total - lastFrames),
                    (unsigned long long)total);
        lastFrames = total;
    }

    std::printf("\nStopping...\n");
    engine.stop();
    std::printf("Total frames routed: %llu\n", (unsigned long long)engine.framesProcessed());

    CoUninitialize();
    return 0;
}
