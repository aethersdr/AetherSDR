// HL2 receive squelch through Hl2RxDsp (#5678 row 1.5).
//
// wdsp_channel_test pins the WdspChannel half — which WDSP stage runs per mode,
// the level maps, and that the stage really gates audio. This pins what that
// test cannot see: that the operator's pair gets from Hl2RxDsp to the channel
// and SURVIVES everything this class does to its channel. Each assertion reads
// Hl2RxDsp::appliedSquelch(), which forwards the channel's record of what it
// wrote to WDSP rather than this class's copy of the request.
//
//   1. A request made before any channel exists is held and lands at configure().
//   2. A mode change moves the squelch to the new family's stage, and CW has none.
//   3. configure() — which REPLACES Config — keeps the squelch (a rate change).
//   4. The asynchronous rebuild: a squelch change made WHILE a background build
//      runs is not pushed at the old channel, and lands on the new one at the
//      swap, on the mode that was set during the build too.
//
// Offline: no socket, no radio. Builds real WDSP channels.

#include "core/backends/hl2/Hl2RxDsp.h"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

using namespace AetherSDR::hl2;
using Stage = WdspChannel::SquelchStage;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    std::fprintf(cond ? stdout : stderr, "%s: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        ++g_failures;
}

static bool applied(const Hl2RxDsp& dsp, Stage stage, bool on, double threshold)
{
    const auto a = dsp.appliedSquelch();
    if (!a)
        return false;
    const bool match = a->stage == stage && a->fmRun == (on && stage == Stage::Fm)
        && a->amRun == (on && stage == Stage::Am)
        && a->voiceRun == (on && stage == Stage::Voice)
        && std::abs(a->threshold - threshold) < 1e-9;
    if (!match) {
        std::fprintf(stderr, "  applied: stage %d fm %d am %d voice %d threshold %g\n",
                     static_cast<int>(a->stage), a->fmRun, a->amRun, a->voiceRun,
                     a->threshold);
    }
    return match;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = 48000;
    cfg.audioSampleRateHz = 24000;
    cfg.dspBlockSize = 1024;
    cfg.fftSize = 256;
    cfg.mode = WdspChannel::Mode::Usb;

    Hl2RxDsp dsp;
    check(!dsp.appliedSquelch().has_value(), "no channel, nothing applied");

    // 1. Held before configure.
    dsp.setSquelch(true, 40);
    check(dsp.squelchEnabled() && dsp.squelchLevel() == 40, "request is held without a channel");
    std::string err;
    check(dsp.configure(cfg, &err), "configure");
    check(applied(dsp, Stage::Voice, true, 0.0075 * 40),
          "a squelch set before configure() lands on ssql in USB");

    // 2. Mode moves it.
    dsp.setMode(WdspChannel::Mode::Fm);
    check(applied(dsp, Stage::Fm, true, std::pow(10.0, -0.8)), "FM moves it to fmsq");
    dsp.setMode(WdspChannel::Mode::Sam);
    check(applied(dsp, Stage::Am, true, -160.0 + 1.6 * 40), "SAM moves it to amsq");
    dsp.setMode(WdspChannel::Mode::Cwu);
    check(applied(dsp, Stage::None, true, 0.0), "CW runs no squelch stage");
    dsp.setMode(WdspChannel::Mode::Am);
    dsp.setSquelch(true, 70);
    check(applied(dsp, Stage::Am, true, -160.0 + 1.6 * 70), "a level change reaches amsq");
    dsp.setSquelch(false, 70);
    check(applied(dsp, Stage::Am, false, -160.0 + 1.6 * 70), "off stops amsq");
    dsp.setSquelch(true, 70);

    // 3. configure() replaces Config; the Config handed in here knows nothing
    //    about squelch and carries USB, exactly like a caller's default would.
    Hl2RxDsp::Config wider = cfg;
    wider.inputSampleRateHz = 96000;
    check(dsp.configure(wider, &err), "reconfigure at 96 kHz");
    check(applied(dsp, Stage::Voice, true, 0.0075 * 70),
          "configure() keeps the squelch and routes it for the new Config's mode");

    // 4. Asynchronous rebuild with changes made mid-build.
    Hl2RxDsp::Config fast = cfg;
    fast.inputSampleRateHz = 192000;
    fast.mode = WdspChannel::Mode::Usb;
    dsp.beginRebuild(fast);
    const unsigned before = dsp.appliedSquelch()->applications;
    dsp.setMode(WdspChannel::Mode::Fm);
    dsp.setSquelch(true, 90);
    check(dsp.appliedSquelch()->applications == before,
          "nothing is pushed at the old channel while a rebuild runs");
    auto result = Hl2RxDsp::buildChannel(fast, false, 50);
    check(result.channel != nullptr, "background build");
    check(dsp.installRebuiltChannel(std::move(result)), "install");
    check(applied(dsp, Stage::Fm, true, std::pow(10.0, -1.8)),
          "the swap applies the squelch set during the build, on the mode set during it");

    std::printf(g_failures == 0 ? "hl2_rxdsp_squelch_test: all passed\n"
                                : "hl2_rxdsp_squelch_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
