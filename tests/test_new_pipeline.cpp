#include "InputProcessor.hpp"
#include "GroupBus.hpp"
#include "OutputMixer.hpp"
#include "audio_utils.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

using namespace anniaudio::core;

int main(int argc, char** argv)
{
    EnsureComInitializedOnThisThread();

    // Read the output hint from command line; default to an unused VAC cable.
    std::string outputName = (argc > 1) ? argv[1] : "Ext (Virtual Audio Cable)";
    std::string inputName  = "Music (Virtual Audio Cable)";

    InputProcessorConfig inCfg;
    inCfg.name   = "music";
    inCfg.type   = "device";
    inCfg.source = inputName;

    InputProcessor input;
    if (!input.init(inCfg)) {
        std::fprintf(stderr, "[test_new_pipeline] InputProcessor init failed\n");
        return 1;
    }
    if (!input.start()) {
        std::fprintf(stderr, "[test_new_pipeline] InputProcessor start failed\n");
        return 1;
    }

    auto group = std::make_shared<GroupBus>();
    group->addInput(input);
    group->setGain(0.25f);

    OutputMixer output;
    if (!output.init(outputName)) {
        std::fprintf(stderr, "[test_new_pipeline] OutputMixer init ('%s') failed\n", outputName.c_str());
        input.stop();
        return 1;
    }
    output.addGroupBus(group);
    if (!output.start()) {
        std::fprintf(stderr, "[test_new_pipeline] OutputMixer start failed\n");
        input.stop();
        return 1;
    }

    std::fprintf(stderr, "[test_new_pipeline] Running: %s -> GroupBus -> %s for 3 seconds...\n",
                 inputName.c_str(), output.outputName().c_str());
    for (int i = 0; i < 6; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::fprintf(stderr, "[test_new_pipeline] input running=%d output running=%d master=%.2f\n",
                     (int)input.running(), (int)output.running(), output.masterVolume());
    }

    output.stop();
    input.stop();
    std::fprintf(stderr, "[test_new_pipeline] stopped\n");
    return 0;
}
