// A 1 Hz counter cannot be sampled at 1 Hz to decide "did anything arrive".
//
// THE DEFECT, observed live on 2026-09-04 with a healthy stream. During a
// connected session with EP6 arriving continuously, telemetryPollMs oscillated
// 0 -> 500 -> 0 across the run and stream-free replies actually landed: the app
// was emitting port-1025 datagrams through its own healthy stream, which is the
// one thing Config C asserts must never happen. The stream was not stalled --
// the in-band temperature changed on every sample throughout.
//
// The cause is two clocks sampling each other:
//
//   Hl2Backend.h:267    kTelemetryPollStateIntervalMs = 1000   (the tick)
//   MetisClient.cpp:785 kLinkPublishIntervalMs        = 1000   (the mirror)
//
// updateTelemetryPollState() asked whether m_link.rxPackets had changed SINCE
// THE LAST TICK, but that field is only refreshed by linkCountersUpdated at
// 1 Hz. Whenever two ticks fall between two publishes, the second sees an
// unchanged counter and declares StreamStalled on a perfectly healthy stream.
//
// WHY THIS IS A PROOF AND NOT A TUNED SIMULATION. The publish is gated by
// `elapsed() < kLinkPublishIntervalMs`, checked on packet arrival, so its period
// is bounded BELOW by 1000 ms and is in practice a little more. The tick is a
// steady 1000 ms. A sequence that is never faster than the tick and sometimes
// slower must fall behind without bound, so a tick with no intervening publish
// is not a possibility, it is a certainty -- only its arrival time is in doubt.
// The jitter below is therefore deterministic and merely decides WHEN, never
// WHETHER. That is why this test does not depend on a lucky seed, and why the
// 60 s Config C capture that saw nothing was too short to bound the beat rather
// than evidence that there was none.
//
// The legacy predicate is kept here permanently as a NEGATIVE CONTROL. A test
// that only exercised the new rule would pass just as well against a rule that
// never reports a stall at all; asserting that the old one DOES fail on this
// trace is what proves the trace discriminates. Four times in this session a
// correct rule sat behind a passing test with nothing asking it.

#include "core/backends/hl2/Hl2TelemetryCadence.h"

#include <cstdio>
#include <vector>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace {

// The publish instants of MetisClient::linkCountersUpdated, in ms. Never faster
// than the tick, sometimes slower -- the structural fact above. Deterministic
// LCG so the trace is identical on every machine and every run.
std::vector<long long> publishTimes(long long durationMs)
{
    std::vector<long long> out;
    unsigned seed = 12345u;
    for (long long t = 0; t < durationMs; ) {
        out.push_back(t);
        seed = seed * 1103515245u + 12345u;
        t += 1000 + static_cast<long long>((seed >> 16) % 60u);  // >= 1000 ms
    }
    return out;
}

// What the code did BEFORE the fix: compare the mirrored counter against its
// value at the previous tick. Reproduced exactly, including that it consumes
// the previous value.
struct LegacyPredicate {
    unsigned long long seenAtLastTick = 0;
    bool stalled(unsigned long long mirroredCounter)
    {
        const bool changed = mirroredCounter != seenAtLastTick;
        seenAtLastTick = mirroredCounter;
        return !changed;
    }
};

}  // namespace

int main()
{
    constexpr long long kRunMs = 600000;   // 10 minutes of simulated stream
    constexpr long long kTickMs = 1000;
    const auto publishes = publishTimes(kRunMs);

    // ---- the trace: a continuously healthy stream, never once stalled ----
    LegacyPredicate legacy;
    int legacyFalseStalls = 0;
    long long firstLegacyFalseStallAt = -1;
    int newFalseStalls = 0;

    std::size_t next = 0;
    unsigned long long counter = 0;     // rxPackets, advancing on every publish
    long long lastAdvanceMs = 0;

    for (long long now = 0; now < kRunMs; now += kTickMs) {
        // Deliver every publish that is due. EP6 is arriving the whole time, so
        // every publish advances the counter.
        while (next < publishes.size() && publishes[next] <= now) {
            counter += 381;             // ~381 EP6 packets/s at 48 kHz, 1 rx
            lastAdvanceMs = publishes[next];
            ++next;
        }

        if (legacy.stalled(counter)) {
            ++legacyFalseStalls;
            if (firstLegacyFalseStallAt < 0)
                firstLegacyFalseStallAt = now;
        }

        // The rule under test, fed the same trace at the same instants.
        if (hl2LinkStateFor(/*connected=*/true, /*heldByOther=*/false,
                            now - lastAdvanceMs) == Hl2LinkState::StreamStalled)
            ++newFalseStalls;
    }

    // THE NEGATIVE CONTROL. If this ever stops holding, the trace has stopped
    // reproducing the defect and every assertion below is vacuous.
    check(legacyFalseStalls > 0,
          "NEGATIVE CONTROL: the tick-to-tick predicate reports a false stall on "
          "a continuously healthy stream -- if this passes, the test proves nothing");
    std::fprintf(stderr,
                 "  legacy predicate: %d false stalls in %lld s, first at t=%lld ms\n",
                 legacyFalseStalls, kRunMs / 1000, firstLegacyFalseStallAt);

    // ---- THE ASSERTION THAT MATTERS ----
    //
    // This was first wired to the LEGACY predicate and watched failing on this
    // exact trace (17 false stalls in 600 s) before hl2LinkStateFor existed, so
    // the green below is one that has been seen red.
    check(newFalseStalls == 0,
          "the production rule never reports a stall on a healthy stream");
    std::fprintf(stderr, "  hl2LinkStateFor: %d false stalls on the same trace\n",
                 newFalseStalls);

    // ---- but it must still SEE a real stall, or it is merely quiet ----
    //
    // The cheapest way to pass the assertion above is a rule that never reports
    // a stall at all, which would silently delete the feature: StreamStalled is
    // the only state that polls without a visible surface, and the stalled
    // stream is the case the whole thing exists for.
    {
        const long long stopMs = 100000;    // EP6 stops here; no further publishes
        long long detectedAt = -1;
        for (long long now = stopMs; now < stopMs + 20000; now += kTickMs) {
            if (hl2LinkStateFor(true, false, now - stopMs) == Hl2LinkState::StreamStalled) {
                detectedAt = now - stopMs;
                break;
            }
        }
        check(detectedAt >= 0, "a genuine stall IS detected");
        check(detectedAt <= 3500,
              "and within 3.5 s -- the stated cost of the fix, asserted so that a "
              "later threshold change cannot quietly make it minutes");
        std::fprintf(stderr, "  genuine stall detected %lld ms after EP6 stopped\n",
                     detectedAt);
    }

    // ---- the disconnected answers do not consult the duration at all ----
    //
    // Including with an absurd elapsed value: a disconnected app has no stream
    // to have stalled, and reporting StreamStalled there would poll a radio
    // nobody is connected to.
    check(hl2LinkStateFor(false, false, 999999) == Hl2LinkState::NotConnected,
          "disconnected, nobody holding it -> NotConnected whatever the clock says");
    check(hl2LinkStateFor(false, true, 999999) == Hl2LinkState::HeldByOther,
          "disconnected, another client holding it -> HeldByOther");
    check(hl2LinkStateFor(true, true, 0) == Hl2LinkState::Streaming,
          "connected wins over a stale heldByOther flag -- our own stream is the "
          "authority on our own stream");

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_link_state_alias_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
