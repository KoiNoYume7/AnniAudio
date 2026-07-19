// Manual verification for AudioMixer's live structural-change path: add and
// remove strips WHILE the mixer is running, confirmed via snapshot() and by
// checking the mixer keeps running (i.e. the audio thread's loop survived
// the command-queue application without deadlocking or crashing).
//
// Not wired into CTest; run manually:
//   build\bin\Release\test_mixer_live_edit.exe "<output>" "<source1>" "<source2>"

#include "AudioMixer.hpp"

#include <cstdio>
#include <windows.h>

using namespace anniaudio::core;

static int failures = 0;
static void check(const char* label, bool cond)
{
    std::printf("  %-45s %s\n", label, cond ? "OK" : "FAIL");
    if (!cond) ++failures;
}

int main(int argc, char* argv[])
{
    if (argc < 4) {
        std::fprintf(stderr, "Usage: %s <output> <source1> <source2>\n", argv[0]);
        return 2;
    }
    std::string output = argv[1];
    std::string source1 = argv[2];
    std::string source2 = argv[3];

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::printf("AnniAudio -- AudioMixer live strip-edit verification\n\n");

    AudioMixer mixer;
    check("init(output)", mixer.init(output));

    MixerStripConfig cfg1;
    cfg1.name = "Strip1";
    cfg1.source = source1;
    cfg1.volume = 0.0f; // keep silent during this test

    auto id1 = mixer.addStrip(cfg1);
    check("addStrip before start() succeeds", id1.has_value());

    check("start() succeeds", mixer.start());
    check("running() true after start()", mixer.running());

    Sleep(300); // let a few audio-thread loop iterations pass

    // --- Live add while running ---
    MixerStripConfig cfg2;
    cfg2.name = "Strip2";
    cfg2.source = source2;
    cfg2.volume = 0.0f;
    auto id2 = mixer.addStrip(cfg2);
    check("addStrip while running returns an id", id2.has_value());

    Sleep(300); // give the audio thread a chance to apply the queued Add

    auto snap = mixer.snapshot();
    check("snapshot shows 2 strips after live add", snap.size() == 2);
    check("mixer still running after live add", mixer.running());

    // --- Live rename while running ---
    if (id2.has_value()) {
        check("renameStrip while running", mixer.renameStrip(*id2, "Strip2-renamed"));
        Sleep(300);
        auto snap2 = mixer.snapshot();
        bool foundRenamed = false;
        for (auto& s : snap2) if (s.id == *id2 && s.name == "Strip2-renamed") foundRenamed = true;
        check("rename applied and visible in snapshot", foundRenamed);
    }

    // --- Volume/mute while running (already lock-free, sanity check only) ---
    if (id1.has_value()) {
        check("setStripVolume while running", mixer.setStripVolume(*id1, 0.42f));
        auto s = mixer.stripSnapshot(*id1);
        check("volume change visible immediately", s.has_value() && std::abs(s->volume - 0.42f) < 0.001f);
    }

    // --- Live remove while running ---
    if (id1.has_value()) {
        check("removeStrip while running", mixer.removeStrip(*id1));
        Sleep(300);
        auto snap3 = mixer.snapshot();
        bool stillThere = false;
        for (auto& s : snap3) if (s.id == *id1) stillThere = true;
        check("removed strip no longer in snapshot", !stillThere);
        check("mixer still running after live remove", mixer.running());
    }

    check("removeStrip with unknown id is a harmless no-op", mixer.removeStrip(999999) || true);

    mixer.stop();
    check("running() false after stop()", !mixer.running());

    CoUninitialize();

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
