// Waterfall time scale: the lock on ms-per-row, and when it may be replaced.
// Header-only, pure logic: no widget, no clock, no socket.
//
// Two claims are pinned, and they pull against each other.
//
// 1. THE LOCK HOLDS. #106 was a time scale that jumped a line up and down
//    because it followed a cadence measured on every row. Steady rows with
//    arrival jitter, a stalled GUI thread that delivers its queued rows in a
//    bunch, and a pause in the rows must each leave the visible value alone.
//
// 2. THE LOCK IS NOT FINAL. The visible value used to be written once, on the
//    third sample after a reset. A bad start (rows back to back at start-up, a
//    stall inside the first window) was then the scale for the whole session:
//    observed as ms-per-row 21.1 where the rows were 40.1 ms apart, a "1s"
//    label on an event 2.00 s old. The estimate behind the scale converged;
//    the scale never read it again.
//
// The rows here are synthetic timestamps. The window measurement is the
// widget's own arithmetic (newest minus older, over the rows between), with
// the span, the smoothing and the decision taken from the header under test.

#include "gui/WaterfallTimeScaleLock.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <vector>

using namespace AetherSDR;

static int g_total = 0;
static int g_failed = 0;

static void report(const char* name, bool ok)
{
    ++g_total;
    if (!ok) {
        ++g_failed;
        std::printf("[FAIL] %s\n", name);
    }
}

static bool within(float value, float target, float fraction)
{
    return std::fabs(value - target) <= fraction * target;
}

// The part of SpectrumWidget that feeds the helpers: a row history, the
// 500 ms hold-off after a reset, and a per-rate estimate that outlives resets.
struct Scale {
    static constexpr long long kHoldOffMs = 500;

    std::vector<long long> stamps;
    std::map<int, WaterfallTimeScaleEstimate> estimates;
    WaterfallTimeScaleLock lock;
    int rate{100};
    int rowsSinceReset{0};
    long long resumeMs{0};
    float visible{0.0f};
    int adoptions{0};
    int lastAdoptionRow{-1};
    float firstLockValue{0.0f};

    void reset(long long nowMs, int newRate, float lawMsPerRow)
    {
        rate = newRate;
        const auto it = estimates.find(rate);
        visible = it != estimates.end() ? it->second.msPerRow : lawMsPerRow;
        lock = {};
        rowsSinceReset = 0;
        resumeMs = nowMs + kHoldOffMs;
    }

    void row(long long nowMs, bool pacedByScale = false)
    {
        stamps.push_back(nowMs);
        ++rowsSinceReset;
        if (nowMs < resumeMs) {
            return;
        }
        const int span = waterfallTimeScaleSampleSpanRows(
            static_cast<int>(stamps.size()), rowsSinceReset);
        if (span <= 0) {
            return;
        }
        const long long olderMs = stamps[stamps.size() - 1 - span];
        if (nowMs <= olderMs) {
            return;
        }
        const float measured = static_cast<float>(nowMs - olderMs)
            / static_cast<float>(span);
        const auto it = estimates.find(rate);
        const WaterfallTimeScaleEstimate previous =
            it != estimates.end() ? it->second
                                  : WaterfallTimeScaleEstimate{measured, 0};
        const WaterfallTimeScaleEstimate estimate =
            foldWaterfallTimeScaleSample(previous, measured);
        estimates[rate] = estimate;
        if (waterfallTimeScaleShouldAdopt(lock, visible, measured, estimate,
                                          pacedByScale)) {
            visible = estimate.msPerRow;
            if (adoptions == 0) {
                firstLockValue = visible;
            }
            ++adoptions;
            lastAdoptionRow = static_cast<int>(stamps.size()) - 1;
        }
    }

    // `count` rows `intervalMs` apart, the first one interval after `nowMs`.
    long long steady(long long nowMs, int count, long long intervalMs)
    {
        for (int i = 0; i < count; ++i) {
            nowMs += intervalMs;
            row(nowMs);
        }
        return nowMs;
    }
};

// Deterministic jitter in [-range, +range].
struct Jitter {
    unsigned state{12345u};
    int next(int range)
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<int>((state >> 8) % (2u * range + 1u)) - range;
    }
};

// ── The decision itself ─────────────────────────────────────────────────────

static void testFirstLock()
{
    WaterfallTimeScaleLock lock;
    report("first lock: two samples do not reach the scale",
           !waterfallTimeScaleShouldAdopt(lock, 40.0f, 21.0f, {21.0f, 2}, false)
               && !lock.locked);
    report("first lock: the third sample does, and locks",
           waterfallTimeScaleShouldAdopt(lock, 40.0f, 21.0f, {21.0f, 3}, false)
               && lock.locked && lock.driftSamples == 0);

    WaterfallTimeScaleLock paced;
    report("first lock: rows paced by the scale may still complete it",
           waterfallTimeScaleShouldAdopt(paced, 40.0f, 80.0f, {80.0f, 3}, true)
               && paced.locked);
}

static void testRelockNeedsAFullRun()
{
    const WaterfallTimeScaleEstimate elsewhere{40.0f, 500};
    WaterfallTimeScaleLock lock{true, 0};
    bool early = false;
    for (int i = 1; i < kWaterfallTimeScaleDriftSamplesBeforeRelock; ++i) {
        early = early
            || waterfallTimeScaleShouldAdopt(lock, 21.0f, 40.0f, elsewhere, false);
    }
    report("re-lock: one sample short of a full run does not reach the scale",
           !early
               && lock.driftSamples == kWaterfallTimeScaleDriftSamplesBeforeRelock - 1);
    report("re-lock: the sample that completes the run does",
           waterfallTimeScaleShouldAdopt(lock, 21.0f, 40.0f, elsewhere, false)
               && lock.locked && lock.driftSamples == 0);

    // A run is consecutive: one sample back inside the tolerance starts it over.
    WaterfallTimeScaleLock broken{true, 0};
    for (int i = 1; i < kWaterfallTimeScaleDriftSamplesBeforeRelock; ++i) {
        waterfallTimeScaleShouldAdopt(broken, 21.0f, 40.0f, elsewhere, false);
    }
    waterfallTimeScaleShouldAdopt(broken, 21.0f, 21.5f, {21.5f, 500}, false);
    report("re-lock: a sample inside the tolerance starts the run over",
           broken.driftSamples == 0
               && !waterfallTimeScaleShouldAdopt(broken, 21.0f, 40.0f, elsewhere, false));

    // The estimate is elsewhere but the newest measurement disagrees with it:
    // it is still moving, so it is not adopted however long that lasts.
    WaterfallTimeScaleLock moving{true, 0};
    bool adopted = false;
    for (int i = 0; i < 10 * kWaterfallTimeScaleDriftSamplesBeforeRelock; ++i) {
        adopted = adopted
            || waterfallTimeScaleShouldAdopt(moving, 40.0f, 455.0f, {300.0f, 500}, false);
    }
    report("re-lock: an estimate still chasing the measurement is never adopted",
           !adopted && moving.driftSamples == 0);

    // Inside the drift tolerance nothing happens, however long.
    WaterfallTimeScaleLock near{true, 0};
    adopted = false;
    for (int i = 0; i < 1000; ++i) {
        adopted = adopted
            || waterfallTimeScaleShouldAdopt(near, 40.0f, 43.5f, {43.5f, 500}, false);
    }
    report("re-lock: 9 % away is inside the tolerance, forever", !adopted);

    WaterfallTimeScaleLock paced{true, 0};
    adopted = false;
    for (int i = 0; i < 1000; ++i) {
        adopted = adopted
            || waterfallTimeScaleShouldAdopt(paced, 67.0f, 80.0f, {80.0f, 500}, true);
    }
    report("re-lock: rows paced by the scale never drive one",
           !adopted && paced.driftSamples == 0);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    WaterfallTimeScaleLock bad{true, 0};
    adopted = false;
    for (int i = 0; i < 100; ++i) {
        adopted = adopted
            || waterfallTimeScaleShouldAdopt(bad, 0.0f, 40.0f, elsewhere, false)
            || waterfallTimeScaleShouldAdopt(bad, nan, 40.0f, elsewhere, false)
            || waterfallTimeScaleShouldAdopt(bad, 21.0f, nan, elsewhere, false)
            || waterfallTimeScaleShouldAdopt(bad, 21.0f, 40.0f, {nan, 500}, false)
            || waterfallTimeScaleShouldAdopt(bad, 21.0f, 0.0f, {0.0f, 500}, false);
    }
    report("re-lock: a zero or non-finite value is never adopted", !adopted);
}

static void testEstimateAndSpan()
{
    const WaterfallTimeScaleEstimate first =
        foldWaterfallTimeScaleSample({123.0f, 0}, 40.0f);
    report("estimate: the first sample is the measurement",
           first.samples == 1 && first.msPerRow == 40.0f);
    const WaterfallTimeScaleEstimate second =
        foldWaterfallTimeScaleSample({40.0f, 1}, 60.0f);
    report("estimate: a later sample moves it by its weight",
           second.samples == 2 && std::fabs(second.msPerRow - 43.0f) < 0.001f);
    const WaterfallTimeScaleEstimate capped =
        foldWaterfallTimeScaleSample({40.0f, kWaterfallTimeScaleMaxSamples}, 40.0f);
    report("estimate: the sample count is capped",
           capped.samples == kWaterfallTimeScaleMaxSamples);

    report("span: too few rows since the reset measures nothing",
           waterfallTimeScaleSampleSpanRows(1000, kWaterfallTimeScaleMinSampleRows) == 0);
    report("span: the minimum is accepted",
           waterfallTimeScaleSampleSpanRows(1000, kWaterfallTimeScaleMinSampleRows + 1)
               == kWaterfallTimeScaleMinSampleRows);
    report("span: a short history bounds it",
           waterfallTimeScaleSampleSpanRows(5, 1000) == 0
               && waterfallTimeScaleSampleSpanRows(13, 1000) == 12);
    report("span: capped at the sample window",
           waterfallTimeScaleSampleSpanRows(1000, 1000) == kWaterfallTimeScaleSampleRows);
}

// ── Whole sequences of rows ─────────────────────────────────────────────────

// Rows back to back just after the reset, then a steady 40 ms: the first lock
// lands near 21 ms (the observed figure) and the scale must leave it.
static void testBurstAtStartConverges()
{
    Scale scale;
    scale.reset(0, 100, 40.0f);
    long long now = 5;
    for (int i = 0; i < 14; ++i) {
        scale.row(now);
        now += 1;
    }
    now = scale.steady(now, 20, 40);
    report("burst: the stimulus reproduces a bad first lock (well under 40 ms)",
           scale.adoptions == 1 && scale.firstLockValue < 30.0f);
    const int lockRow = scale.lastAdoptionRow;

    now = scale.steady(now, 100, 40);
    report("burst: within 100 rows of the first lock the scale is within 5 % of 40 ms",
           within(scale.visible, 40.0f, 0.05f)
               && scale.lastAdoptionRow - lockRow <= 100);
    report("burst: it got there in one step, not a staircase", scale.adoptions == 2);

    scale.steady(now, 5000, 40);
    report("burst: and then stays put", scale.adoptions == 2);
}

// The opposite error: a stall inside the first window makes the first lock
// far too slow.
static void testStallAtStartConverges()
{
    Scale scale;
    scale.reset(0, 100, 40.0f);
    long long now = scale.steady(0, 12, 40);
    now += 680; // one interval of 720 ms
    now = scale.steady(now, 4, 40);
    report("stall: the stimulus reproduces a bad first lock (well over 40 ms)",
           scale.adoptions == 1 && scale.firstLockValue > 60.0f);
    const int lockRow = scale.lastAdoptionRow;

    now = scale.steady(now, 100, 40);
    report("stall: within 100 rows of the first lock the scale is within 5 % of 40 ms",
           within(scale.visible, 40.0f, 0.05f)
               && scale.lastAdoptionRow - lockRow <= 100);
    report("stall: it got there in one step", scale.adoptions == 2);
    scale.steady(now, 5000, 40);
    report("stall: and then stays put", scale.adoptions == 2);
}

// Why the lock exists. Each row lands up to 10 ms early or late.
static void testJitterNeverReachesTheScale()
{
    Scale scale;
    Jitter jitter;
    scale.reset(0, 100, 40.0f);
    for (int i = 1; i <= 6000; ++i) {
        scale.row(40LL * i + jitter.next(10));
    }
    report("jitter: 6000 rows at 40 +/- 10 ms lock once and never again",
           scale.adoptions == 1 && within(scale.visible, 40.0f, 0.05f));
}

// A GUI thread that stalls and then delivers its queued rows in a bunch, and
// rows that simply stop for a while. One interrupted stretch at a time.
static void testAStallOrAPauseNeverReachesTheScale()
{
    for (const long long stallMs : {120LL, 200LL, 240LL, 280LL, 320LL, 400LL,
                                    800LL, 2000LL}) {
        Scale scale;
        scale.reset(0, 100, 40.0f);
        long long now = scale.steady(0, 200, 40);
        for (int cycle = 0; cycle < 20; ++cycle) {
            // The rows the stall held back arrive 1 ms apart when it ends.
            const int held = static_cast<int>(stallMs / 40);
            now += stallMs;
            for (int i = 0; i < held; ++i) {
                scale.row(now + i);
            }
            now += stallMs - held * 40; // back on the 40 ms grid
            now = scale.steady(now, 200, 40);
        }
        char name[96];
        std::snprintf(name, sizeof name,
                      "stall of %lld ms, rows bunched after it: locks once", stallMs);
        report(name, scale.adoptions == 1 && within(scale.visible, 40.0f, 0.05f));
    }

    for (const long long pauseMs : {160LL, 200LL, 240LL, 280LL, 320LL, 500LL,
                                    1000LL, 10000LL, 60000LL}) {
        Scale scale;
        scale.reset(0, 100, 40.0f);
        long long now = scale.steady(0, 200, 40);
        for (int cycle = 0; cycle < 20; ++cycle) {
            now += pauseMs; // no rows at all, then they resume
            now = scale.steady(now, 200, 40);
        }
        char name[96];
        std::snprintf(name, sizeof name,
                      "pause of %lld ms with no rows: locks once", pauseMs);
        report(name, scale.adoptions == 1 && within(scale.visible, 40.0f, 0.05f));
    }
}

// A rate change resets the scale: the new rate measures from scratch, and a
// rate measured before comes back from its own estimate.
static void testRateChangeRecalibrates()
{
    Scale scale;
    scale.reset(0, 100, 40.0f);
    long long now = scale.steady(0, 300, 40);
    report("rate: settled at 40 ms before the change",
           scale.adoptions == 1 && within(scale.visible, 40.0f, 0.02f));

    // The law for the new rate is wrong by a third; the rows say 67 ms.
    scale.reset(now, 60, 50.0f);
    report("rate: the change unlocks the scale and seeds it from the law",
           !scale.lock.locked && scale.lock.driftSamples == 0
               && scale.visible == 50.0f);
    const int rowsBefore = static_cast<int>(scale.stamps.size());
    now = scale.steady(now, 40, 67);
    report("rate: the new cadence is on the scale within 40 rows",
           scale.adoptions == 2 && within(scale.visible, 67.0f, 0.05f)
               && scale.lastAdoptionRow - rowsBefore < 40);
    now = scale.steady(now, 2000, 67);
    report("rate: and stays put", scale.adoptions == 2);

    scale.reset(now, 100, 12.0f);
    report("rate: a rate measured before is seeded from its estimate, not the law",
           within(scale.visible, 40.0f, 0.02f) && !scale.lock.locked);
    scale.steady(now, 300, 40);
    report("rate: and re-locks there",
           scale.adoptions == 3 && within(scale.visible, 40.0f, 0.05f));
}

// A cadence change with NO reset: where a row is one FFT frame, an FFT FPS
// change moves the cadence and the rate control is untouched (25 -> 12 fps).
static void testCadenceChangeWithoutResetRecalibrates()
{
    Scale scale;
    scale.reset(0, 100, 40.0f);
    long long now = scale.steady(0, 300, 40);
    const int changeRow = static_cast<int>(scale.stamps.size());
    now = scale.steady(now, 100, 83);
    report("cadence: 40 -> 83 ms with no reset is on the scale within 100 rows",
           scale.adoptions == 2 && within(scale.visible, 83.0f, 0.05f)
               && scale.lastAdoptionRow - changeRow <= 100);
    now = scale.steady(now, 2000, 83);
    report("cadence: and stays put", scale.adoptions == 2);

    Jitter jitter;
    const int backRow = static_cast<int>(scale.stamps.size());
    for (int i = 1; i <= 2000; ++i) {
        scale.row(now + 40LL * i + jitter.next(8));
    }
    report("cadence: back to 40 ms, with jitter, in one step",
           scale.adoptions == 3 && within(scale.visible, 40.0f, 0.05f)
               && scale.lastAdoptionRow - backRow <= 100);
}

// Fallback and TX rows are emitted on a deadline of now + the visible value,
// checked when an FFT frame arrives, so they come out at that value rounded up
// to the next frame. A scale that followed them would be following itself.
static void testRowsPacedByTheScaleDoNotWalkIt()
{
    Scale scale;
    Jitter jitter;
    scale.reset(0, 60, 67.0f);
    long long nextRowMs = 0;
    for (int frame = 1; frame <= 20000; ++frame) {
        const long long now = 40LL * frame + jitter.next(3);
        if (now >= nextRowMs) {
            scale.row(now, true);
            nextRowMs = now + std::lround(scale.visible);
        }
    }
    report("paced rows: the scale locks once and is not walked upward",
           scale.adoptions == 1 && scale.visible < 100.0f);
}

int main()
{
    testFirstLock();
    testRelockNeedsAFullRun();
    testEstimateAndSpan();
    testBurstAtStartConverges();
    testStallAtStartConverges();
    testJitterNeverReachesTheScale();
    testAStallOrAPauseNeverReachesTheScale();
    testRateChangeRecalibrates();
    testCadenceChangeWithoutResetRecalibrates();
    testRowsPacedByTheScaleDoNotWalkIt();

    std::printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed ? 1 : 0;
}
