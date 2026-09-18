// #2554 (Overview tab) -- UiTickLagMeter: the "actual - nominal" arithmetic behind
// the GUI tick-lag card and chart, driven with CONSTRUCTED timestamps through
// tickAt(). No timer, no widget, no socket; the numbers stand for no captured
// session and exercise the accumulation and reset rules only.
#include "gui/UiTickLagMeter.h"

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_TRUE(cond, what) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, what); \
        ++g_failures; \
    } \
} while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }
static constexpr qint64 kMs = 1'000'000;   // ns per ms

int main()
{
    // 1. Nothing measured before two ticks: the first tick is the baseline.
    {
        UiTickLagMeter meter;
        UiTickLagMeter::Reading r = meter.take();
        EXPECT_TRUE(r.tickCount == 0 && near(r.lagMaxMs, 0.0), "a fresh meter has nothing to report");
        meter.tickAt(0);
        r = meter.take();
        EXPECT_TRUE(r.tickCount == 0, "the baseline tick records no lag");
    }

    // 2. On-time ticks read zero lag; late ticks read their excess over 50 ms.
    {
        UiTickLagMeter meter;
        meter.tickAt(0);
        meter.tickAt(50 * kMs);     // exactly nominal: 0
        meter.tickAt(100 * kMs);    // 0
        meter.tickAt(170 * kMs);    // 70 ms gap: 20 ms late
        meter.tickAt(220 * kMs);    // 0
        const UiTickLagMeter::Reading r = meter.take();
        EXPECT_TRUE(r.tickCount == 4, "four intervals since the baseline");
        EXPECT_TRUE(near(r.lagMaxMs, 20.0), "worst lag is the 20 ms late tick");
        EXPECT_TRUE(near(r.lagMeanMs, 5.0), "mean lag is 20/4");
        EXPECT_TRUE(near(r.spanMs, 220.0), "span is the wall time covered");
    }

    // 3. An early tick (a timer that fired ahead of schedule) floors at zero
    //    rather than crediting negative lag against later late ticks.
    {
        UiTickLagMeter meter;
        meter.tickAt(0);
        meter.tickAt(40 * kMs);     // 10 ms early: 0, not -10
        meter.tickAt(100 * kMs);    // 60 ms gap: 10 late
        const UiTickLagMeter::Reading r = meter.take();
        EXPECT_TRUE(near(r.lagMaxMs, 10.0) && near(r.lagMeanMs, 5.0), "early ticks floor at zero");
    }

    // 4. take() resets the accumulation but keeps the baseline: the next
    //    interval is measured from the last tick, not from the read.
    {
        UiTickLagMeter meter;
        meter.tickAt(0);
        meter.tickAt(150 * kMs);    // 100 late
        meter.take();
        meter.tickAt(200 * kMs);    // 50 gap from the last tick: 0 late
        const UiTickLagMeter::Reading r = meter.take();
        EXPECT_TRUE(r.tickCount == 1 && near(r.lagMaxMs, 0.0), "after take(), the next interval is measured from the last tick");
    }

    // 5. reset() drops the baseline too: the tick after it records nothing,
    //    so a long unread stretch (the dialog closed) never lands in the first
    //    reading after reopening.
    {
        UiTickLagMeter meter;
        meter.tickAt(0);
        meter.tickAt(60 * 60 * 1000 * kMs);   // an hour later
        meter.reset();
        meter.tickAt(60 * 60 * 1000 * kMs + 5000 * kMs);   // 5 s after that: would read 4950 late
        const UiTickLagMeter::Reading r = meter.take();
        EXPECT_TRUE(r.tickCount == 0, "the first tick after reset() is a baseline, not a 5 s lag");
    }

    if (g_failures) {
        std::fprintf(stderr, "ui_tick_lag_meter_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ui_tick_lag_meter_test: all checks passed\n");
    return 0;
}
