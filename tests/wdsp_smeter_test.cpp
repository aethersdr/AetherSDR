// WdspSMeter.h: the S-meter arithmetic shared by Hl2RxDsp/AnanRxDsp (the tap
// gate) and Hl2Backend/AnanBackend (the publisher). Pure, clock injected, so
// the 100 ms publish tick is pinned without waiting on a wall clock.

#include "core/dsp/WdspSMeter.h"

#include <array>
#include <cmath>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main()
{
    constexpr int kDspRate = 48000;
    constexpr int kBlock = 1024;
    constexpr std::array<int, 6> kRatesKsps{48, 96, 192, 384, 768, 1536};

    // ---- settle window ----
    check(WdspSMeter::settleBlocks(48000, kBlock) == 15,
          "3 tau of the 0.100 s average at 48 ksps / 1024 is 15 blocks (ceil 14.0625)");
    for (const int ksps : kRatesKsps) {
        const int n = WdspSMeter::settleBlocks(ksps * 1000, kBlock);
        const double seconds = static_cast<double>(n) * kBlock / (ksps * 1000.0);
        const double target = WdspSMeter::kSettleTaus * WdspSMeter::kAverageTauSec;
        const double oneBlock = static_cast<double>(kBlock) / (ksps * 1000.0);
        check(seconds >= target && seconds - target <= oneBlock,
              "the settle rounds up to the same 0.3 s at every input rate");
    }
    check(WdspSMeter::settleBlocks(0, 0) == 1 && WdspSMeter::settleBlocks(-1, kBlock) == 1,
          "a degenerate geometry still swallows one block rather than dividing by zero");

    // ---- read cadence ----
    check(WdspSMeter::emitEveryBlocks(48000, kDspRate) == 1,
          "at the DSP rate every block is read");
    check(WdspSMeter::emitEveryBlocks(96000, kDspRate) == 2,
          "at 96 ksps every second block is read");
    check(WdspSMeter::emitEveryBlocks(1536000, kDspRate) == 32,
          "at 1536 ksps every 32nd block is read -- ~47 readings/s, not ~1500");
    check(WdspSMeter::emitEveryBlocks(0, kDspRate) == 1
              && WdspSMeter::emitEveryBlocks(48000, 0) == 1
              && WdspSMeter::emitEveryBlocks(24000, kDspRate) == 1,
          "a degenerate or sub-DSP-rate geometry reads every block");

    // ---- the tap gate: settle, then cadence ----
    {
        WdspSMeterTap tap;
        // Never armed: reads every block, from the first.
        check(tap.tick() && tap.tick(), "an unarmed tap reads every block");

        tap.arm(1536000, kBlock, kDspRate);
        const int settle = WdspSMeter::settleBlocks(1536000, kBlock);
        // 0.3 s is exactly 450 blocks here; 3 * 0.1 is not exactly 0.3 in
        // binary, so the ceil may land one block over. Within one block of
        // 0.3 s is the contract, and the same bound the per-rate loop above
        // pins.
        check(settle == 450 || settle == 451,
              "1536 ksps / 1024 settles for 450 blocks (0.3 s), or one over");
        int reads = 0;
        for (int i = 0; i < settle; ++i)
            if (tap.tick()) ++reads;
        check(reads == 0, "nothing is read during the settle window");
        check(tap.settleBlocksRemaining() == 0, "the window is spent exactly at its length");
        reads = 0;
        for (int i = 0; i < 31; ++i)
            if (tap.tick()) ++reads;
        check(reads == 0, "after the settle, the first 31 blocks at 1536 ksps are not read");
        check(tap.tick(), "the 32nd is");
        reads = 0;
        for (int i = 0; i < 320; ++i)
            if (tap.tick()) ++reads;
        check(reads == 10, "then exactly one read per 32 blocks -- the DSP-rate cadence");

        // Re-arming resets both the window and the cadence phase; it does not
        // accumulate, so a rate change that arms twice waits one window.
        tap.arm(48000, kBlock, kDspRate);
        tap.arm(48000, kBlock, kDspRate);
        reads = 0;
        for (int i = 0; i < 15; ++i)
            if (tap.tick()) ++reads;
        check(reads == 0 && tap.settleBlocksRemaining() == 0,
              "arming twice waits one window, not two");
        check(tap.tick(), "at 48 ksps the first block after the settle is read");
    }

    // ---- the publisher: smooth every reading, publish on the tick ----
    {
        SMeterSmoother sm;
        auto out = sm.feedAt(-73.0, 0);
        check(out.has_value() && std::fabs(*out - (-73.0)) < 1e-9,
              "the first reading is published at once, taken whole");
        out = sm.feedAt(-53.0, 10);
        check(!out.has_value(), "a reading inside the 100 ms interval is not published");
        check(std::fabs(sm.value() - (-63.0)) < 1e-9,
              "but it is smoothed: a rise takes attack 0.5, -73 then -53 reads -63");
        out = sm.feedAt(-83.0, 20);
        check(!out.has_value() && std::fabs(sm.value() - (-66.0)) < 1e-9,
              "a fall takes decay 0.15: -66, so the needle falls more slowly than it rises");
        out = sm.feedAt(-66.0, 99);
        check(!out.has_value(), "99 ms after the last publish is still inside the interval");
        out = sm.feedAt(-66.0, 100);
        check(out.has_value() && std::fabs(*out - (-66.0)) < 1e-9,
              "100 ms after the last publish the smoothed value is published");
        out = sm.feedAt(-66.0, 150);
        check(!out.has_value(), "the interval is measured from the last PUBLISH, not the last feed");

        sm.reset();
        out = sm.feedAt(-90.0, 151);
        check(out.has_value() && std::fabs(*out - (-90.0)) < 1e-9,
              "after reset the next reading is taken whole and published at once");
    }

    // The production entry point runs the same arithmetic on its own clock:
    // the first feed publishes whatever the clock says.
    {
        SMeterSmoother sm;
        check(sm.feed(-50.0).has_value(), "feed() publishes the first reading at once");
        check(std::fabs(sm.value() - (-50.0)) < 1e-9, "and takes it whole");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "wdsp_smeter_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
