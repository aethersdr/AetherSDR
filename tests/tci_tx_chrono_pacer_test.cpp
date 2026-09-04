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
    // 21.333 ms period, 5 ms poll. Four quiet polls then one emit.
    for (int i = 0; i < 20; ++i) {
        const TciTxChronoPacer::TickResult r =
            pacer.tick(5 * 1000000LL, /*audioGapNs=*/i * 5 * 1000000LL);
        if (!expect(r.framesToSend <= 1, "steady poll never bursts")
            || !expect(r.droppedNs == 0, "steady poll drops nothing")
            || !expect(r.wouldHaveSent <= 1, "steady poll would_have_sent<=1")) {
            return false;
        }
        if (r.framesToSend == 1) {
            pacer.onChronoSent();
            ++sent;
            pacer.onAudioBlock(); // client answers promptly
        }
    }
    return expect(sent >= 4 && sent <= 5, "about one chrono per 21.3 ms");
}

bool testStallDoesNotReplayUnbounded()
{
    TciTxChronoPacer pacer;
    // 53 ms GUI stall (KN7K median uiHeartbeat). Old while would send 2–3.
    const TciTxChronoPacer::TickResult r =
        pacer.tick(53 * 1000000LL, /*audioGapNs=*/-1);
    if (!expect(r.framesToSend == 1, "stall emits at most one chrono")
        || !expect(r.wouldHaveSent >= 2, "telemetry records the unbounded count")
        || !expect(r.droppedNs > 0, "excess stall is dropped not banked")) {
        std::printf("got frames=%d would=%d dropped=%lld\n",
                    r.framesToSend, r.wouldHaveSent,
                    static_cast<long long>(r.droppedNs));
        return false;
    }
    pacer.onChronoSent();
    if (!expect(pacer.outstanding() == 1, "one unanswered after stall tick")) {
        return false;
    }
    return true;
}

bool testLongStallStillOneFrame()
{
    TciTxChronoPacer pacer;
    // 1675 ms outlier from KN7K. Old while would send ~78 frames.
    const TciTxChronoPacer::TickResult r =
        pacer.tick(1675 * 1000000LL, /*audioGapNs=*/-1);
    return expect(r.framesToSend == 1, "1.6 s stall still one frame")
        && expect(r.wouldHaveSent >= 70, "would_have_sent captures the old burst")
        && expect(r.droppedNs > TciTxChronoPacer::kPeriodNs,
                  "most of the stall is discarded");
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
    return expect(resumed.framesToSend == 1, "audio reply unblocks chrono");
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
        && expect(forgot.framesToSend == 1, "chrono resumes after forget")
        && expect(pacer.outstanding() == 0
                      || forgot.outstanding == 0,
                  "outstanding cleared before the new send");
}

bool testAudioBlockDecrementsOutstanding()
{
    TciTxChronoPacer pacer;
    pacer.tick(TciTxChronoPacer::kPeriodNs, -1);
    pacer.onChronoSent();
    pacer.onChronoSent(); // defensive: startTxChrono sends one immediately
    pacer.onAudioBlock();
    pacer.onAudioBlock();
    pacer.onAudioBlock(); // extra reply must not underflow
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
        || !testStallDoesNotReplayUnbounded()
        || !testLongStallStillOneFrame()
        || !testOutstandingCapRefusesMoreChrono()
        || !testForgetStuckOutstanding()
        || !testAudioBlockDecrementsOutstanding()
        || !testResetClearsState()) {
        return 1;
    }
    std::printf("tci_tx_chrono_pacer_test passed\n");
    return 0;
}
