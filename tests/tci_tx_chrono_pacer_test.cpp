#include "core/TciTxChronoPacer.h"

#include <cstdio>

using AetherSDR::TciTxChronoPacer;

namespace {

bool expect(bool cond, const char* what)
{
    if (!cond) {
        std::printf("FAIL: %s\n", what);
    }
    return cond;
}

bool testSteadyPollEmitsEveryPeriod()
{
    TciTxChronoPacer pacer;
    int sent = 0;
    for (int i = 0; i < 20; ++i) {
        const TciTxChronoPacer::TickResult r =
            pacer.tick(5 * 1000000LL, /*audioGapNs=*/i * 5 * 1000000LL);
        if (!expect(r.framesToSend <= 1, "steady 5 ms poll never catch-up bursts")
            || !expect(r.droppedNs == 0, "steady poll drops nothing")) {
            return false;
        }
        for (int n = 0; n < r.framesToSend; ++n) {
            pacer.onChronoSent();
            ++sent;
            pacer.onAudioBlock();
        }
    }
    return expect(sent >= 4 && sent <= 5, "about one chrono per 21.3 ms");
}

bool testLateGuiPollDoesNotDrop()
{
    TciTxChronoPacer pacer;
    // 5K Studio Display: timer fires at ~40 ms, not 5 ms.
    int sent = 0;
    std::int64_t elapsed = 0;
    for (int i = 0; i < 10; ++i) {
        const TciTxChronoPacer::TickResult r =
            pacer.tick(40 * 1000000LL, /*audioGapNs=*/2 * 1000000LL);
        elapsed += 40 * 1000000LL;
        if (!expect(r.droppedNs == 0, "40 ms poll must not drop owed time")
            || !expect(r.framesToSend >= 1 && r.framesToSend <= 3,
                       "40 ms poll pays 1–2 frames, not zero")) {
            std::printf("tick %d frames=%d dropped=%lld would=%d\n",
                        i, r.framesToSend,
                        static_cast<long long>(r.droppedNs), r.wouldHaveSent);
            return false;
        }
        for (int n = 0; n < r.framesToSend; ++n) {
            pacer.onChronoSent();
            ++sent;
            pacer.onAudioBlock();
        }
    }
    const double hz = static_cast<double>(sent) * TciTxChronoPacer::kStereoFrames
        / (static_cast<double>(elapsed) / 1.0e9);
    if (!expect(hz > 45000.0 && hz < 51000.0, "late polls still average ~48 kHz")) {
        std::printf("effective48k=%.1f sent=%d elapsed_ms=%.1f\n",
                    hz, sent, elapsed / 1.0e6);
        return false;
    }
    return true;
}

bool testFiftyMsStallPaysTwoNotOne()
{
    TciTxChronoPacer pacer;
    const TciTxChronoPacer::TickResult r =
        pacer.tick(53 * 1000000LL, /*audioGapNs=*/-1);
    if (!expect(r.framesToSend == 2, "53 ms stall pays two periods")
        || !expect(r.wouldHaveSent == 2, "would_have_sent matches owed")
        || !expect(r.droppedNs == 0, "53 ms fits in the 213 ms backlog")) {
        std::printf("got frames=%d would=%d dropped=%lld\n",
                    r.framesToSend, r.wouldHaveSent,
                    static_cast<long long>(r.droppedNs));
        return false;
    }
    return true;
}

bool testLongStallIsBoundedNotDroppedToOne()
{
    TciTxChronoPacer pacer;
    // 1675 ms outlier. Unbounded while ≈ 78 frames; we send 3 and keep ~213 ms.
    const TciTxChronoPacer::TickResult r =
        pacer.tick(1675 * 1000000LL, /*audioGapNs=*/-1);
    if (!expect(r.framesToSend == TciTxChronoPacer::kMaxFramesPerTick,
                "long stall still bounded per tick")
        || !expect(r.wouldHaveSent >= 70, "telemetry records the unbounded count")
        || !expect(r.droppedNs > TciTxChronoPacer::kPeriodNs,
                   "seconds of stall are discarded after keeping ~213 ms")) {
        std::printf("got frames=%d would=%d dropped=%lld accum=%lld\n",
                    r.framesToSend, r.wouldHaveSent,
                    static_cast<long long>(r.droppedNs),
                    static_cast<long long>(r.accumNs));
        return false;
    }
    return expect(pacer.accumNs() == TciTxChronoPacer::kMaxBacklogNs,
                  "backlog sits at the cap after a watchdog stall");
}

bool testOutstandingCapRefusesMoreChrono()
{
    TciTxChronoPacer pacer;
    for (int i = 0; i < TciTxChronoPacer::kMaxOutstanding; ++i) {
        const TciTxChronoPacer::TickResult r =
            pacer.tick(TciTxChronoPacer::kPeriodNs, /*audioGapNs=*/0);
        if (!expect(r.framesToSend == 1, "fill outstanding with one per period")) {
            return false;
        }
        pacer.onChronoSent();
    }
    const TciTxChronoPacer::TickResult blocked =
        pacer.tick(TciTxChronoPacer::kPeriodNs, /*audioGapNs=*/10 * 1000000LL);
    if (!expect(blocked.framesToSend == 0, "no chrono while at outstanding cap")
        || !expect(blocked.outstandingCapped, "outstanding_capped is set")) {
        return false;
    }
    pacer.onAudioBlock();
    const TciTxChronoPacer::TickResult resumed =
        pacer.tick(TciTxChronoPacer::kPeriodNs, /*audioGapNs=*/5 * 1000000LL);
    return expect(resumed.framesToSend >= 1, "audio reply unblocks chrono");
}

bool testForgetStuckOutstanding()
{
    TciTxChronoPacer pacer;
    const TciTxChronoPacer::TickResult first =
        pacer.tick(TciTxChronoPacer::kPeriodNs, /*audioGapNs=*/-1);
    if (!expect(first.framesToSend == 1, "first chrono")) {
        return false;
    }
    pacer.onChronoSent();
    const TciTxChronoPacer::TickResult forgot =
        pacer.tick(TciTxChronoPacer::kPeriodNs,
                   TciTxChronoPacer::kForgetStuckNs);
    return expect(forgot.forgotStuck, "250 ms without audio forgets outstanding")
        && expect(forgot.framesToSend >= 1, "chrono resumes after forget");
}

bool testAudioBlockDecrementsOutstanding()
{
    TciTxChronoPacer pacer;
    pacer.tick(TciTxChronoPacer::kPeriodNs, -1);
    pacer.onChronoSent();
    pacer.onChronoSent();
    pacer.onAudioBlock();
    pacer.onAudioBlock();
    pacer.onAudioBlock();
    return expect(pacer.outstanding() == 0, "outstanding floors at zero");
}

bool testResetClearsState()
{
    TciTxChronoPacer pacer;
    pacer.tick(80 * 1000000LL, -1);
    pacer.onChronoSent();
    pacer.reset();
    return expect(pacer.accumNs() == 0, "reset accum")
        && expect(pacer.outstanding() == 0, "reset outstanding");
}

} // namespace

int main()
{
    if (!testSteadyPollEmitsEveryPeriod()
        || !testLateGuiPollDoesNotDrop()
        || !testFiftyMsStallPaysTwoNotOne()
        || !testLongStallIsBoundedNotDroppedToOne()
        || !testOutstandingCapRefusesMoreChrono()
        || !testForgetStuckOutstanding()
        || !testAudioBlockDecrementsOutstanding()
        || !testResetClearsState()) {
        return 1;
    }
    std::printf("tci_tx_chrono_pacer_test passed\n");
    return 0;
}
