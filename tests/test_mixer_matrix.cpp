#include "AudioMixerMatrix.hpp"
#include "audio_utils.hpp"

#include <cstdio>
#include <thread>
#include <chrono>

#include <windows.h>

using namespace anniaudio::core;

int main(int argc, char** argv)
{
    EnsureComInitializedOnThisThread();

    std::string outputName = (argc > 1) ? argv[1] : "Ext (Virtual Audio Cable)";
    std::string inputName  = (argc > 2) ? argv[2] : "Music (Virtual Audio Cable)";

    std::printf("[test_mixer_matrix] Output: %s\n", outputName.c_str());
    std::printf("[test_mixer_matrix] Input:  %s\n", inputName.c_str());

    AudioMixerMatrix matrix;

    if (!matrix.addOutput(outputName)) {
        std::fprintf(stderr, "[test_mixer_matrix] addOutput('%s') failed\n", outputName.c_str());
        return 1;
    }

    InputConfig in;
    in.name = "music";
    in.type = "device";
    in.source = inputName;
    auto iid = matrix.addInput(in);
    if (!iid) {
        std::fprintf(stderr, "[test_mixer_matrix] addInput failed\n");
        return 1;
    }

    GroupConfig g;
    g.name = "Music";
    g.inputIds = { *iid };
    g.outputIds = { outputName };
    g.volume = 1.0f;
    auto gid = matrix.addGroup(g);
    if (!gid) {
        std::fprintf(stderr, "[test_mixer_matrix] addGroup failed\n");
        return 1;
    }

    if (!matrix.start()) {
        std::fprintf(stderr, "[test_mixer_matrix] start failed\n");
        return 1;
    }

    std::printf("[test_mixer_matrix] Running for 5 seconds...\n");
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto s = matrix.snapshot();
        std::printf("[test_mixer_matrix] t=%d running=%d outputs=%zu groups=%zu inputs=%zu\n",
                    i + 1, s.running ? 1 : 0, s.outputs.size(), s.groups.size(), s.inputs.size());
        for (const auto& o : s.outputs) {
            std::printf("  output '%s' master=%.1f%% peak=%.4f rms=%.4f groups=[",
                        o.name.c_str(), o.master, o.masterPeak, o.masterRms);
            for (auto id : o.groupIds) std::printf("%u ", id);
            std::printf("]\n");
        }
        for (const auto& gr : s.groups) {
            std::printf("  group '%s' id=%u volume=%.1f%% muted=%d peak=%.4f rms=%.4f\n",
                        gr.name.c_str(), gr.id, gr.volume, gr.muted ? 1 : 0, gr.peak, gr.rms);
        }
        for (const auto& in : s.inputs) {
            std::printf("  input '%s' id=%u peak=%.4f rms=%.4f\n",
                        in.name.c_str(), in.id, in.peak, in.rms);
        }
    }

    matrix.stop();
    std::printf("[test_mixer_matrix] stopped\n");
    return 0;
}
