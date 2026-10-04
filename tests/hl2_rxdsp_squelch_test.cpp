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
//   5. (below 4) A squelch change the channel REFUSES converges: it is held as
//      pending and applied at the next IQ block, without another setSquelch.
//   4. The asynchronous rebuild: a squelch change made WHILE a background build
//      runs is not pushed at the old channel, and lands on the new one at the
//      swap, on the mode that was set during the build too.
//   6. A non-finite level offset is dropped, not retried at the block rate.
//   7. THE BACKEND HALF. An LNA change on Hl2Backend re-pushes the squelch on
//      the I/O thread, so the gate moves dB for dB with the LNA (#6092).
//
// Offline: no socket, no radio. Builds real WDSP channels.

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2DbReference.h"
#include "core/backends/hl2/Hl2RxDsp.h"

#include <QCoreApplication>

#include <cmath>
#include <complex>
#include <vector>
#include <cstdio>
#include <string>
#include <optional>
#include <utility>

namespace AetherSDR::hl2 {

// Receiver 0 gets a configured chain on the backend's I/O thread, as
// hl2_apf_agc_off_test does, so the push travels its production queued path.
struct Hl2SquelchTestAccess {
    static Hl2RxDsp* attachChain(Hl2Backend& backend, const Hl2RxDsp::Config& cfg)
    {
        auto* dsp = new Hl2RxDsp();
        std::string err;
        if (!dsp->configure(cfg, &err)) {
            delete dsp;
            return nullptr;
        }
        dsp->moveToThread(backend.m_ioThread);
        backend.m_rx[0].dsp = dsp;
        backend.publishIoDsps();
        return dsp;
    }
    static void tearDown(Hl2Backend& backend) { backend.tearDownReceivers(); }
};

}  // namespace AetherSDR::hl2

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
        && a->amRun == (on && stage == Stage::Level)
        && std::abs(a->threshold - threshold) < 1e-9;
    if (!match) {
        std::fprintf(stderr, "  applied: stage %d fm %d am %d threshold %g\n",
                     static_cast<int>(a->stage), a->fmRun, a->amRun, a->threshold);
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
    dsp.setSquelch(true, 40, 0.0);
    check(dsp.squelchEnabled() && dsp.squelchLevel() == 40, "request is held without a channel");
    std::string err;
    check(dsp.configure(cfg, &err), "configure");
    check(applied(dsp, Stage::Level, true, -140.0 + 0.7 * 40),
          "a squelch set before configure() lands on amsq in USB");

    // 2. Mode moves it.
    dsp.setMode(WdspChannel::Mode::Fm);
    check(applied(dsp, Stage::Fm, true, std::pow(10.0, -0.8)), "FM moves it to fmsq");
    dsp.setMode(WdspChannel::Mode::Sam);
    check(applied(dsp, Stage::Level, true, -140.0 + 0.7 * 40), "SAM moves it to amsq");
    dsp.setMode(WdspChannel::Mode::Cwu);
    check(applied(dsp, Stage::None, false, 0.0), "CW runs no squelch stage");
    dsp.setMode(WdspChannel::Mode::Am);
    dsp.setSquelch(true, 70, 0.0);
    check(applied(dsp, Stage::Level, true, -140.0 + 0.7 * 70), "a level change reaches amsq");
    dsp.setSquelch(true, 0, 0.0);
    check(applied(dsp, Stage::Level, false, -140.0), "level 0 stops amsq (open)");
    dsp.setSquelch(false, 70, 0.0);
    check(applied(dsp, Stage::Level, false, -140.0 + 0.7 * 70), "off stops amsq");
    dsp.setSquelch(true, 70, 0.0);

    // 3. configure() replaces Config; the Config handed in here knows nothing
    //    about squelch and carries USB, exactly like a caller's default would.
    Hl2RxDsp::Config wider = cfg;
    wider.inputSampleRateHz = 96000;
    check(dsp.configure(wider, &err), "reconfigure at 96 kHz");
    check(applied(dsp, Stage::Level, true, -140.0 + 0.7 * 70),
          "configure() keeps the squelch and routes it for the new Config's mode");
    // The LNA referral rides with the pair and survives configure() (#6092).
    dsp.setSquelch(true, 70, 32.0);
    check(applied(dsp, Stage::Level, true, -140.0 + 0.7 * 70 + 32.0),
          "the level offset reaches amsq");
    check(dsp.configure(wider, &err)
              && applied(dsp, Stage::Level, true, -140.0 + 0.7 * 70 + 32.0),
          "configure() keeps the level offset");
    dsp.setSquelch(true, 70, 0.0);

    // 4. Asynchronous rebuild with changes made mid-build.
    Hl2RxDsp::Config fast = cfg;
    fast.inputSampleRateHz = 192000;
    fast.mode = WdspChannel::Mode::Usb;
    dsp.beginRebuild(fast);
    const unsigned before = dsp.appliedSquelch()->applications;
    dsp.setMode(WdspChannel::Mode::Fm);
    dsp.setSquelch(true, 90, 0.0);
    check(dsp.appliedSquelch()->applications == before,
          "nothing is pushed at the old channel while a rebuild runs");
    auto result = Hl2RxDsp::buildChannel(fast, false, 50);
    check(result.channel != nullptr, "background build");
    check(dsp.installRebuiltChannel(std::move(result)), "install");
    check(applied(dsp, Stage::Fm, true, std::pow(10.0, -1.8)),
          "the swap applies the squelch set during the build, on the mode set during it");

    // 5. Refused, then converges on the next IQ block.
    dsp.setSquelch(true, 30, 0.0);
    check(applied(dsp, Stage::Fm, true, std::pow(10.0, -0.6)) && !dsp.squelchPending(),
          "baseline FM/30 applied");
    const unsigned beforeRefusal = dsp.appliedSquelch()->applications;
    dsp.refuseChannelControlForTest(1);
    dsp.setSquelch(true, 60, 0.0);
    check(dsp.squelchPending(), "a refused squelch is marked pending");
    check(dsp.appliedSquelch()->applications == beforeRefusal
              && applied(dsp, Stage::Fm, true, std::pow(10.0, -0.6)),
          "the refused request did not reach WDSP (the channel still runs FM/30)");
    // One EP6-sized block of silence; the retry runs before it is processed.
    dsp.processIqBlock(std::vector<std::complex<float>>(126));
    check(!dsp.squelchPending(), "the next IQ block clears pending");
    check(applied(dsp, Stage::Fm, true, std::pow(10.0, -1.2)),
          "the next IQ block applies the refused request (FM/60)");
    // A refusal that persists keeps retrying until the channel takes it.
    dsp.refuseChannelControlForTest(3);
    dsp.setSquelch(false, 60, 0.0);
    dsp.processIqBlock(std::vector<std::complex<float>>(126));
    dsp.processIqBlock(std::vector<std::complex<float>>(126));
    check(dsp.squelchPending(), "still pending while the channel keeps refusing");
    dsp.processIqBlock(std::vector<std::complex<float>>(126));
    check(!dsp.squelchPending() && applied(dsp, Stage::Fm, false, std::pow(10.0, -1.2)),
          "converges once the channel accepts (squelch off)");

    // 6. A non-finite offset is invalid, not busy: the rest of the request
    //    lands at the last good offset and nothing is left pending.
    dsp.setMode(WdspChannel::Mode::Usb);
    dsp.setSquelch(true, 50, 32.0);
    dsp.setSquelch(true, 60, std::nan(""));
    check(!dsp.squelchPending() && dsp.squelchLevelOffsetDb() == 32.0
              && applied(dsp, Stage::Level, true, -140.0 + 0.7 * 60 + 32.0),
          "a NaN offset keeps the last good offset and is not retried");

    // 7. Through Hl2Backend: an LNA change re-pushes the squelch offset.
    {
        Hl2Backend backend;
        Hl2RxDsp* chain = Hl2SquelchTestAccess::attachChain(backend, cfg);
        check(chain != nullptr, "receiver 0 has a configured chain on the I/O thread");
        // Read on the chain's thread; the blocking call also drains the
        // queued pushes ahead of it.
        const auto onChain = [chain]() {
            std::optional<WdspChannel::AppliedSquelch> a;
            QMetaObject::invokeMethod(chain, [&]() { a = chain->appliedSquelch(); },
                                      Qt::BlockingQueuedConnection);
            return a;
        };
        const auto gateAt = [](int lnaDb, int level) {
            Hl2DbReference ref;
            ref.setLnaGainDb(lnaDb);
            return -140.0 + 0.7 * level + ref.levelSquelchOffsetDb();
        };
        const int lna = backend.lnaEffectiveDb();
        backend.setSliceSquelch(0, true, 50);
        auto a = onChain();
        check(a && a->stage == Stage::Level && a->amRun
                  && std::abs(a->threshold - gateAt(lna, 50)) < 1e-9,
              "the slice's squelch lands on amsq at the current LNA's offset");
        backend.setLnaAutoOffsetDb(10);
        check(backend.lnaEffectiveDb() == lna - 10, "the automatic offset lowers the LNA 10 dB");
        a = onChain();
        check(a && a->amRun && std::abs(a->threshold - (gateAt(lna, 50) - 10.0)) < 1e-9,
              "an LNA change re-pushes the squelch: the gate moves with it");
        backend.setLnaAutoOffsetDb(0);
        a = onChain();
        check(a && std::abs(a->threshold - gateAt(lna, 50)) < 1e-9,
              "and moves back when the LNA is restored");
        Hl2SquelchTestAccess::tearDown(backend);
    }

    std::printf(g_failures == 0 ? "hl2_rxdsp_squelch_test: all passed\n"
                                : "hl2_rxdsp_squelch_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
