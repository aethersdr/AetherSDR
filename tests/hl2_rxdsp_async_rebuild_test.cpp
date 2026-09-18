// A PAN-BANDWIDTH REBUILD MUST NOT COST THE OPERATOR ANYTHING THEY SET.
//
// Hl2Backend rebuilds every receiver's WDSP chain when the DDC rate changes,
// because that register is radio-wide. Since the build moved onto a dedicated
// thread (Hl2RxDsp::buildChannel / beginRebuild / installRebuiltChannel), two
// new ways to lose operator state appeared, and both are SILENT:
//
//   1. buildChannel() runs off this object's thread and is handed only a
//      Config. Everything Config does not carry -- the notch set, the noise
//      blanker, the slice shift -- has to be re-applied at the swap by
//      installChannel(). A rebuild that quietly drops the operator's notches
//      looks exactly like a working rebuild.
//
//   2. A control verb arriving WHILE the build runs must not push at WDSP --
//      that would block the I/O thread on the process-wide setup mutex the
//      build is holding, which is the starvation the whole change exists to
//      remove -- so it updates this object's mirrors only. If the swap then
//      fails to replay those mirrors, the operator's change is simply gone,
//      with the UI still showing it.
//
// This pins both, plus the failure path: a build that never lands must leave
// the running chain and the deferral state exactly as they were.
//
// NOT A TIMING TEST. Nothing here measures that the rebuild is faster or that
// audio survives it; that needs a radio. This is about what the swap CARRIES.

#include "core/backends/hl2/Hl2RxDsp.h"

#include <QCoreApplication>

#include <cstdio>
#include <string>
#include <thread>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static Hl2RxDsp::Config configAt(int rateHz)
{
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = rateHz;
    cfg.audioSampleRateHz = 24000;   // AudioEngine's native RX rate
    cfg.dspBlockSize = 1024;
    cfg.fftSize = 1024;
    cfg.mode = WdspChannel::Mode::Usb;
    cfg.filterLowHz = 150.0;
    cfg.filterHighHz = 3000.0;
    cfg.agcMode = 3;
    cfg.maximumAgcGainDb = 39.0;
    cfg.blockForOutput = true;
    return cfg;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Hl2RxDsp dsp;
    std::string err;
    check(dsp.configure(configAt(48000), &err),
          err.empty() ? "initial 48 kHz configure() succeeds" : err.c_str());

    // The state a rebuild has to carry, set the way Hl2Backend sets it.
    dsp.setNotchTuneFrequency(14'200'000.0);
    dsp.addNotch(0, 14'201'000.0, 100.0, true);
    dsp.addNotch(1, 14'202'000.0, 100.0, true);
    dsp.setNoiseBlanker(true, 42);
    dsp.setShift(1200.0);
    check(dsp.notchCount() == 2, "two notches in the mirror before the rebuild");
    check(dsp.wdspNotchCount() == 2, "and WDSP holds the same two");
    check(dsp.appliedNoiseBlankerLevel() == 42,
          "the blanker request landed before the rebuild");

    // ── A rebuild in flight defers, it does not lose ──────────────────────
    const Hl2RxDsp::Config wide = configAt(384000);
    dsp.beginRebuild(wide);
    const bool nbOn = dsp.noiseBlankerEnabled();
    const int nbLevel = dsp.noiseBlankerLevel();

    // Everything below arrives while the build is notionally running.
    dsp.setMode(WdspChannel::Mode::Am);
    dsp.setFilter(-4000.0, 4000.0);
    dsp.setAgc(2, 51.0);
    dsp.addNotch(2, 14'203'000.0, 100.0, true);
    dsp.setNoiseBlanker(true, 77);
    dsp.setShift(0.0);

    const WdspChannel::Config* live = dsp.channelConfig();
    check(live != nullptr, "the old channel is still there during the build");
    if (live) {
        check(live->inputSampleRate == 48000,
              "the RUNNING channel is untouched by a build in flight");
        check(live->mode == WdspChannel::Mode::Usb,
              "a mid-build setMode() does not reach the running channel");
        check(live->agcMode == 3,
              "a mid-build setAgc() does not reach the running channel");
    }
    check(dsp.notchCount() == 3, "a mid-build notch is taken by the mirror");
    check(dsp.wdspNotchCount() == 2,
          "and is NOT pushed at WDSP while the build holds the setup mutex");
    check(dsp.appliedNoiseBlankerLevel() == 42,
          "a mid-build blanker change is held, not applied to the old channel");

    // ── The build itself, on a thread that is not this object's ───────────
    Hl2RxDsp::RebuildResult result;
    std::thread builder([&] { result = Hl2RxDsp::buildChannel(wide, nbOn, nbLevel); });
    builder.join();
    check(result.channel != nullptr,
          result.error.empty() ? "buildChannel() works off the owning thread"
                               : result.error.c_str());

    check(dsp.installRebuiltChannel(std::move(result)),
          "installRebuiltChannel() takes a completed build");

    // ── Everything the operator asked for is on the new chain ─────────────
    live = dsp.channelConfig();
    check(live != nullptr, "a channel is installed after the swap");
    if (live) {
        check(live->inputSampleRate == 384000, "the new channel runs at the new rate");
        check(live->mode == WdspChannel::Mode::Am,
              "the mid-build mode change is replayed onto the new channel");
        check(live->filterLowHz == -4000.0 && live->filterHighHz == 4000.0,
              "the mid-build passband is replayed onto the new channel");
        check(live->agcMode == 2 && live->maximumAgcGainDb == 51.0,
              "the mid-build AGC is replayed onto the new channel");
    }
    // outputBlockSize = dspBlockSize * audioRate / inputRate.
    check(dsp.channelOutputBlockSize() == 1024u * 24000u / 384000u,
          "the audio block is the new rate's size");
    check(dsp.notchCount() == 3 && dsp.wdspNotchCount() == 3,
          "every notch, including the one added mid-build, is on the new chain");
    check(dsp.appliedNoiseBlankerLevel() == 77 && dsp.appliedNoiseBlankerEnabled(),
          "the mid-build blanker change lands at the swap");

    // ── A build that never lands changes nothing ──────────────────────────
    //
    // This is what removes the roll-back from Hl2Backend::applyPanBandwidth():
    // the failure path is "destroy the new set and return", so the running
    // chain must survive a failed build completely intact.
    Hl2RxDsp::Config bad = configAt(192000);
    bad.dspBlockSize = 0;   // rejected at buildChannel()'s guard
    dsp.beginRebuild(bad);
    const Hl2RxDsp::RebuildResult failed = [&] {
        Hl2RxDsp::RebuildResult r;
        std::thread t([&] { r = Hl2RxDsp::buildChannel(bad, false, 0); });
        t.join();
        return r;
    }();
    check(failed.channel == nullptr && !failed.error.empty(),
          "a malformed Config fails the build instead of opening a channel");
    dsp.abandonRebuild();

    live = dsp.channelConfig();
    check(live != nullptr && live->inputSampleRate == 384000,
          "the running chain survives a failed build untouched");
    check(dsp.notchCount() == 3 && dsp.wdspNotchCount() == 3,
          "and so does its notch set");

    // abandonRebuild() must actually re-open the control path, or every later
    // notch and mode change would silently stop reaching WDSP.
    dsp.addNotch(3, 14'204'000.0, 100.0, true);
    check(dsp.notchCount() == 4 && dsp.wdspNotchCount() == 4,
          "control verbs reach WDSP again once the rebuild is abandoned");

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_rxdsp_async_rebuild_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
