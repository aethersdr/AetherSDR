// The automatic receive-gain control law, exercised as a pure function.
//
// The whole reason `Hl2AutoGainPolicy.h` has no Qt, no socket and no clock is
// that the behaviours worth arguing about here are timing-dependent, and a
// timing-dependent decision reachable only by running a radio into a strong
// band is a decision nobody re-checks. Every property below is EXHAUSTIVE over
// something — every (samples, overload) pair, every window phase, every plant
// threshold — because with elapsed time as an input there is no clock to be at
// the mercy of.
//
// Hl2Backend evaluates the same functions rather than its own copy, so what
// passes here is what the radio runs.

#include "core/backends/hl2/Hl2AutoGainPolicy.h"

#include <cstdio>
#include <vector>

using AetherSDR::hl2::AutoGainAction;
using AetherSDR::hl2::AutoGainConfig;
using AetherSDR::hl2::AutoGainObservation;
using AetherSDR::hl2::AutoGainReason;
using AetherSDR::hl2::AutoGainState;
using AetherSDR::hl2::AutoGainWindow;
using AetherSDR::hl2::autoGainStep;
using AetherSDR::hl2::binaryHighLowConfig;
using AetherSDR::hl2::probingReleaseConfig;
using AetherSDR::hl2::classifyWindow;
using AetherSDR::hl2::bandscopeReleaseConfig;
using AetherSDR::hl2::BandscopeHeadroom;
using AetherSDR::hl2::HeadroomObservation;
using AetherSDR::hl2::gatedPeakBiasDbForPeriod;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

// A synthetic plant: the converter rails whenever the offset is below
// `clipBelowOffsetDb`. Not the radio — see the note at the end of this file for
// what that means and does not mean.
struct Plant {
    int clipBelowOffsetDb = 0;
    int samplesPerWindow = 19;   // ~19 raddr-0 observations per 100 ms at 48 kHz

    [[nodiscard]] int overloadFor(int offsetDb) const
    {
        return offsetDb < clipBelowOffsetDb ? samplesPerWindow : 0;
    }
};

// A plant whose knee MOVES, which is the entire reason probing exists. Before
// `dawnWindow` the converter rails unless the loop is holding at least
// `eveningClipBelowOffsetDb`; after it, unless it is holding
// `daytimeClipBelowOffsetDb`. ON8ST's #5535 objection predicts that difference
// is 10-20 dB on a real antenna.
struct DiurnalPlant {
    int eveningClipBelowOffsetDb = 18;
    int daytimeClipBelowOffsetDb = 0;
    int dawnWindow = 1 << 30;
    int samplesPerWindow = 19;   // ~19 raddr-0 observations per 100 ms at 48 kHz

    [[nodiscard]] int clipBelow(int window) const
    {
        return window < dawnWindow ? eveningClipBelowOffsetDb
                                   : daytimeClipBelowOffsetDb;
    }
    [[nodiscard]] int overloadFor(int window, int offsetDb) const
    {
        return offsetDb < clipBelow(window) ? samplesPerWindow : 0;
    }
};

struct ProbeRun {
    AutoGainState state;
    int clippedWindows = 0;      // windows in which the CONVERTER actually railed
    int probes = 0;              // releases issued
    int failedProbes = 0;        // attacks that arrived while a probe was in flight
    int windowsProbeInFlight = 0;  // how long the loop spent clipped after a probe
    int firstZeroOffsetWindow = -1;
    int firstProbeWindow = -1;
    int windowsClippedAfter = 0;   // clipped windows at or after `countClipsFrom`
    std::vector<std::int64_t> intervalAfterFailure;
    std::vector<int> offsetTrace;
};

// Closed loop against a moving plant. `keyedPeriodWindows`/`keyedForWindows`
// impose a transmit duty cycle so the post-unkey hold-off is exercised in the
// same loop rather than in a separate hand-built state.
struct ProbeDrive {
    int windows = 1000;
    std::int64_t windowMs = 100;
    int ceiling = 24;
    int keyedPeriodWindows = 0;   // 0 = never keyed
    int keyedForWindows = 0;
    int countClipsFrom = 0;
};

ProbeRun probeRun(const AutoGainConfig& cfg, const DiurnalPlant& plant,
                  const ProbeDrive& drive, AutoGainState start = AutoGainState{})
{
    ProbeRun r;
    AutoGainState st = start;
    bool probeInFlight = false;
    std::int64_t sinceUnkey = -1;
    for (int i = 0; i < drive.windows; ++i) {
        const bool keyed = drive.keyedPeriodWindows > 0
                        && (i % drive.keyedPeriodWindows) >= (drive.keyedPeriodWindows
                                                              - drive.keyedForWindows);
        AutoGainObservation obs;
        obs.samples = plant.samplesPerWindow;
        // A keyed radio hears its own transmitter: the bit is slammed and says
        // nothing about the antenna.
        obs.overloadSamples = keyed ? plant.samplesPerWindow
                                    : plant.overloadFor(i, st.offsetDb);
        obs.elapsedMs = drive.windowMs;
        obs.availableOffsetDb = drive.ceiling;
        obs.resetWarmup = (i == 0);
        obs.keyed = keyed;
        obs.msSinceUnkey = sinceUnkey;
        if (keyed) {
            sinceUnkey = 0;
        } else if (sinceUnkey >= 0) {
            sinceUnkey += drive.windowMs;
        }

        if (!keyed && obs.overloadSamples > 0) {
            ++r.clippedWindows;
            if (i >= drive.countClipsFrom) ++r.windowsClippedAfter;
            if (probeInFlight) ++r.windowsProbeInFlight;
        }

        const AutoGainAction a = autoGainStep(st, obs, cfg);
        if (a.deltaDb < 0) {
            ++r.probes;
            if (r.firstProbeWindow < 0) r.firstProbeWindow = i;
            probeInFlight = true;
        } else if (a.deltaDb > 0) {
            if (probeInFlight) {
                ++r.failedProbes;
                r.intervalAfterFailure.push_back(a.next.dwellRequiredMs);
            }
            probeInFlight = false;
        }
        // A probe that has been believed is no longer in flight.
        if (probeInFlight && !a.next.releasedSinceTrip) {
            probeInFlight = false;
        }
        st = a.next;
        r.offsetTrace.push_back(st.offsetDb);
        if (st.offsetDb == 0 && i >= drive.countClipsFrom
            && r.firstZeroOffsetWindow < 0) {
            r.firstZeroOffsetWindow = i;
        }
    }
    r.state = st;
    return r;
}

struct RunResult {
    AutoGainState state;
    int attacks = 0;
    int releases = 0;
    int lastMovementWindow = -1;
    int maxOffset = 0;
    int minOffset = 0;
    bool everAboveCeiling = false;
    bool everNegative = false;
    bool everActedWhileKeyed = false;
    bool floorWarned = false;
    std::vector<int> offsetTrace;
};

// Drive `windows` windows of `windowMs` each through the law, with the plant
// answering from the CURRENT offset — a closed loop, not a replayed script.
RunResult run(const AutoGainConfig& cfg, const Plant& plant, int windows,
              std::int64_t windowMs, int ceiling = 26,
              AutoGainState start = AutoGainState{})
{
    RunResult r;
    AutoGainState st = start;
    r.minOffset = st.offsetDb;
    for (int i = 0; i < windows; ++i) {
        AutoGainObservation obs;
        obs.samples = plant.samplesPerWindow;
        obs.overloadSamples = plant.overloadFor(st.offsetDb);
        obs.elapsedMs = windowMs;
        obs.availableOffsetDb = ceiling;
        obs.resetWarmup = (i == 0);
        const AutoGainAction a = autoGainStep(st, obs, cfg);
        if (a.deltaDb > 0) { ++r.attacks; r.lastMovementWindow = i; }
        if (a.deltaDb < 0) { ++r.releases; r.lastMovementWindow = i; }
        if (a.warnFloorOnce) r.floorWarned = true;
        // `next` already carries the new offset; `deltaDb` is what the BACKEND
        // hands to setLnaAutoOffsetDb, and the two must agree. Assert that here
        // rather than assuming it, then take `next` verbatim.
        if (a.next.offsetDb != st.offsetDb + a.deltaDb) {
            r.everAboveCeiling = true;   // reported as a safety violation below
        }
        st = a.next;
        if (st.offsetDb > ceiling) r.everAboveCeiling = true;
        if (st.offsetDb < 0) r.everNegative = true;
        if (st.offsetDb > r.maxOffset) r.maxOffset = st.offsetDb;
        if (st.offsetDb < r.minOffset) r.minOffset = st.offsetDb;
        r.offsetTrace.push_back(st.offsetDb);
    }
    r.state = st;
    return r;
}

}  // namespace

int main()
{
    const AutoGainConfig kDefault;

    // ---- 1. DENOMINATOR HONESTY -------------------------------------------
    //
    // Exhaustive over every (samples, overloadSamples) pair a small window can
    // present. A window with too few observations is Void WHATEVER the
    // numerator says: "three of three railed" is not 100 % clipping, it is
    // three samples. Void is not Clean and it is not Hot.
    {
        bool everActedOnThinEvidence = false;
        bool voidEverMisread = false;
        for (int samples = 0; samples <= 40; ++samples) {
            for (int over = 0; over <= samples; ++over) {
                const AutoGainWindow w = classifyWindow(samples, over, kDefault);
                if (samples < kDefault.minSamples && w != AutoGainWindow::Void) {
                    voidEverMisread = true;
                }
                if (samples < kDefault.minSamples) {
                    AutoGainState st;
                    st.offsetDb = 5;
                    st.warmupRemaining = 0;
                    AutoGainObservation obs;
                    obs.samples = samples;
                    obs.overloadSamples = over;
                    obs.elapsedMs = 100;
                    const auto a = autoGainStep(st, obs, kDefault);
                    if (a.deltaDb != 0) everActedOnThinEvidence = true;
                }
            }
        }
        check(!voidEverMisread,
              "every window below the minimum denominator classifies as Void, "
              "whatever its numerator");
        check(!everActedOnThinEvidence,
              "and a Void window can neither attack nor release");
    }

    // ---- 2. THE THREE STATES ARE THE THREE STATES -------------------------
    {
        check(classifyWindow(19, 0, kDefault) == AutoGainWindow::Clean,
              "no assertions in a full window is Clean");
        check(classifyWindow(19, 1, kDefault) == AutoGainWindow::Marginal,
              "one assertion in nineteen is Marginal, not Hot");
        check(classifyWindow(19, 9, kDefault) == AutoGainWindow::Marginal,
              "nine of nineteen is still not more often than not");
        check(classifyWindow(19, 10, kDefault) == AutoGainWindow::Hot,
              "ten of nineteen is railing more often than not");
        check(classifyWindow(3, 3, kDefault) == AutoGainWindow::Void,
              "three of three is Void — thin evidence, not 100 % clipping");
    }

    // ---- 3. SAFETY, over a long closed-loop run at every plant threshold ---
    //
    // For every X the loop can reach, no sequence puts the offset outside
    // [0, ceiling]. The offset is what becomes an attenuation below the
    // operator's baseline, so this is the property that stops the loop making
    // the radio louder than they asked or deafer than they allowed.
    {
        bool everOut = false;
        for (int x = 0; x <= 30; ++x) {
            const RunResult r = run(kDefault, Plant{x}, 600, 100, 26);
            if (r.everAboveCeiling || r.everNegative) everOut = true;
        }
        check(!everOut,
              "over 31 plants x 600 windows the offset never leaves [0, ceiling]");
    }

    // ---- 4. TRANSMIT ------------------------------------------------------
    //
    // The HL2 hears its own transmitter. Nothing observed while keyed, or
    // inside the measured post-unkey settling window, is about the antenna.
    {
        bool actedWhileKeyed = false;
        bool actedInHoldoff = false;
        for (std::int64_t sinceUnkey = 0; sinceUnkey < 600; sinceUnkey += 7) {
            AutoGainState st;
            st.offsetDb = 6;
            st.warmupRemaining = 0;
            AutoGainObservation obs;
            obs.samples = 19;
            obs.overloadSamples = 19;   // slammed, as a transmission does
            obs.elapsedMs = 100;
            obs.keyed = true;
            if (autoGainStep(st, obs, kDefault).deltaDb != 0) actedWhileKeyed = true;
            obs.keyed = false;
            obs.msSinceUnkey = sinceUnkey;
            const auto a = autoGainStep(st, obs, kDefault);
            if (sinceUnkey < kDefault.unkeyHoldoffMs && a.deltaDb != 0) {
                actedInHoldoff = true;
            }
        }
        check(!actedWhileKeyed, "no action is ever taken while keyed");
        check(!actedInHoldoff,
              "and none within the 300 ms post-unkey hold-off — a bound measured "
              "on this station (FIND-16 / d83-unkey-transient), not borrowed");
        // The boundary is the value itself, and it opens rather than closes.
        AutoGainState st;
        st.offsetDb = 6;
        st.warmupRemaining = 3;
        AutoGainObservation obs;
        obs.samples = 19;
        obs.overloadSamples = 19;
        obs.elapsedMs = 100;
        obs.msSinceUnkey = kDefault.unkeyHoldoffMs;
        const auto a = autoGainStep(st, obs, kDefault);
        check(a.reason == AutoGainReason::Warmup,
              "at exactly 300 ms the hold-off has ended and warm-up takes over — "
              "the loop still does not act on the first evidence after a transition");
    }

    // ---- 5. NEVER RELEASE ON ABSENCE --------------------------------------
    //
    // This is the consequence of the gateware finding that has to be code
    // rather than prose: the observation only exists while the radio is
    // streaming, so when the evidence stops the loop HOLDS. It must not decay
    // back toward the operator's baseline on the strength of having heard
    // nothing, and it must not bank the silence as dwell either.
    {
        AutoGainState st;
        st.offsetDb = 12;
        st.warmupRemaining = 0;
        st.cleanMs = 2900;          // one window short of releasing
        int releases = 0;
        bool everStale = false;
        std::int64_t cleanAfter = 0;
        for (int i = 0; i < 400; ++i) {   // 40 seconds of nothing
            AutoGainObservation obs;
            obs.samples = 0;             // the stream stopped
            obs.elapsedMs = 100;
            const auto a = autoGainStep(st, obs, kDefault);
            if (a.deltaDb < 0) ++releases;
            if (a.reason == AutoGainReason::Stale) everStale = true;
            st = a.next;
            cleanAfter = st.cleanMs;
        }
        check(releases == 0,
              "forty seconds of Void windows produce ZERO release steps");
        check(st.offsetDb == 12, "and the offset is HELD, not decayed");
        check(cleanAfter == 2900,
              "and the release dwell is neither advanced nor reset by silence");
        check(everStale,
              "and the state reports itself stale — a dead overload bit is not a "
              "clean converter");
    }

    // ---- 6. THE DITHER CASE, EXHAUSTIVE OVER PHASE ------------------------
    //
    // The failure the shared cooldown exists to stop: alternating hot/clean
    // across window boundaries re-entering "first observation, act now" over
    // and over and railing the offset in a few hundred milliseconds.
    //
    // Exhaustive over BOTH phase offsets that matter — which window the hot one
    // lands on, and the window length relative to the cooldown — because with
    // elapsed time as an input there is no scheduler to blame.
    //
    // The range is deliberately made non-binding (a large ceiling and a 1 dB
    // step) so that the ONLY thing bounding the attack count is the cooldown.
    {
        AutoGainConfig cfg = kDefault;
        cfg.maxOffsetDb = 100000;
        cfg.attackStepDb = 1;
        cfg.firstStepHotDb = 1;
        cfg.firstStepMarginalDb = 1;
        cfg.tripMarginDb = 0;
        cfg.tripMarginGrowthDb = 0;

        bool everExceeded = false;
        int worstAttacks = 0;
        std::int64_t worstT = 0;
        for (std::int64_t windowMs = 1; windowMs <= 250; ++windowMs) {
            for (int phase = 0; phase < 2; ++phase) {
                AutoGainState st;
                st.warmupRemaining = 0;
                int attacks = 0;
                std::int64_t t = 0;
                const int windows = static_cast<int>(20000 / windowMs) + 1;
                for (int i = 0; i < windows; ++i) {
                    AutoGainObservation obs;
                    obs.samples = 19;
                    obs.overloadSamples = ((i + phase) % 2 == 0) ? 19 : 0;
                    obs.elapsedMs = windowMs;
                    obs.availableOffsetDb = 100000;
                    const auto a = autoGainStep(st, obs, cfg);
                    if (a.deltaDb > 0) ++attacks;
                    st = a.next;
                    t += windowMs;
                }
                // ONE IMMEDIATE STEP, THEN ONE PER COOLDOWN. The "+1" is not
                // slack in the bound, it is the design: the first threat after
                // a quiet period is answered at once rather than made to wait
                // out a window, because that is exactly the interval during
                // which the front end is being slammed and nothing has acted
                // yet. What the shared cooldown must prevent is a SECOND free
                // step -- chatter across a window boundary re-entering that
                // fast path over and over and railing the offset in a few
                // hundred milliseconds.
                //
                // This bound was found by the test rather than assumed: the
                // first version wrote T/cooldown and the law returned 101 in
                // 20001 ms. The extra step was the immediate first response,
                // which is correct, so the assertion moved rather than the law.
                const std::int64_t bound = 1 + t / cfg.attackCooldownMs;
                if (attacks > bound) {
                    everExceeded = true;
                    if (attacks > worstAttacks) { worstAttacks = attacks; worstT = t; }
                }
            }
        }
        check(!everExceeded,
              "across 500 phase/window-length combinations, the attack count never "
              "exceeds one immediate step plus one per shared cooldown");
        if (everExceeded) {
            std::printf("       worst: %d attacks in %lld ms\n",
                        worstAttacks, static_cast<long long>(worstT));
        }
    }

    // ---- 7. CONVERGENCE, and it is the point ------------------------------
    //
    // Against a plant that clips whenever the offset is below X, for EVERY X
    // the range can reach: the loop settles somewhere that is not clipping, is
    // not needlessly deaf, and then STOPS MOVING. A loop with no memory of
    // where it clipped does not have this property — it re-probes into a known
    // wall forever — so this is the test that says the design is better than
    // reacting to a bare event, rather than merely as good.
    {
        bool everStillMoving = false;
        bool everClippingAtRest = false;
        bool everNeedlesslyDeaf = false;
        int worstX = -1;
        for (int x = 0; x <= 26; ++x) {
            // 300 s: long enough for several trip/release cycles, short enough
            // that tripForgetMs (300 s) does not reset the memory mid-run.
            const RunResult r = run(kDefault, Plant{x}, 2500, 100, 26);
            const int settleFrom = 2000;
            if (r.lastMovementWindow >= settleFrom) {
                everStillMoving = true;
                if (worstX < 0) worstX = x;
            }
            if (r.state.offsetDb < x) everClippingAtRest = true;
            if (r.state.offsetDb > x + kDefault.firstStepHotDb
                                     + kDefault.tripMarginMaxDb) {
                everNeedlesslyDeaf = true;
            }
        }
        check(!everStillMoving,
              "for every plant threshold 0..26 the loop stops moving and stays "
              "stopped for the last 50 s of a 250 s run");
        if (everStillMoving) {
            std::printf("       first non-converging plant: X = %d\n", worstX);
        }
        check(!everClippingAtRest,
              "and it comes to rest at an offset that is not clipping");
        check(!everNeedlesslyDeaf,
              "and no deeper than one rate-sized step plus the memory's margin "
              "below the point where clipping stops");
    }

    // ---- 8. WITHOUT THE MEMORY IT HUNTS FOREVER ---------------------------
    //
    // The negative control for property 7. Same law, same plant, memory
    // disabled: it must NOT converge. If this passes with the memory off, then
    // property 7 was proving something other than what it claims.
    {
        AutoGainConfig noMemory = kDefault;
        noMemory.tripMarginDb = 0;
        noMemory.tripMarginGrowthDb = 0;
        noMemory.tripMarginMaxDb = 0;
        const RunResult r = run(noMemory, Plant{10}, 2500, 100, 26);
        check(r.lastMovementWindow >= 2000,
              "CONTROL: with the trip memory disabled the same loop on the same "
              "plant is still moving at the end of the run — the memory is what "
              "converges it, not the dwell");
        std::printf("       (memory off: %d attacks, %d releases; "
                    "memory on: see property 7)\n", r.attacks, r.releases);
    }

    // ---- 9. THE BINARY CONTROLLER IS A CONFIGURATION, NOT A REWRITE -------
    //
    // ON8ST's own sweep puts the entire 0 % -> 86 % clipping transition inside
    // six decibels, against a smallest useful step of three. If the bench
    // confirms that, the right control is a per-band high/low decision with a
    // long hold rather than a ramp. That control is THIS function under
    // `binaryHighLowConfig` — same state machine, same guards, same tests.
    {
        const AutoGainConfig bin = binaryHighLowConfig(26, 60000);
        const RunResult r = run(bin, Plant{10}, 4000, 100, 26);
        bool onlyTwoStates = true;
        for (const int o : r.offsetTrace) {
            if (o != 0 && o != 26) onlyTwoStates = false;
        }
        check(onlyTwoStates,
              "under the binary configuration the offset only ever holds one of "
              "two values — it is a switch, and nothing in the law had to change");
        check(r.attacks >= 1 && r.state.offsetDb == 26,
              "it switches to the low-gain state on the first real evidence and "
              "holds there while the band keeps clipping");
        // And the hold is what stops it dithering: over 400 s against a plant
        // that is clean at the low state, it must not flap.
        const RunResult quiet = run(bin, Plant{0}, 4000, 100, 26);
        check(quiet.attacks == 0 && quiet.releases == 0,
              "and on a band that never clips it never leaves the high-gain state");
    }

    // ---- 10. THE FLOOR IS SURFACED, NOT SILENTLY ENDURED ------------------
    {
        // A plant that clips at every reachable offset: the front end needs
        // attenuation ahead of the radio and no amount of LNA will supply it.
        const RunResult r = run(kDefault, Plant{99}, 1000, 100, 26);
        check(r.state.offsetDb == 26, "an unreachable plant drives the offset to the floor");
        check(r.floorWarned,
              "and the loop says so ONCE rather than attacking a clamp forever");
        int warnings = 0;
        AutoGainState st;
        st.offsetDb = 26;
        st.warmupRemaining = 0;
        for (int i = 0; i < 1000; ++i) {
            AutoGainObservation obs;
            obs.samples = 19;
            obs.overloadSamples = 19;
            obs.elapsedMs = 100;
            obs.availableOffsetDb = 26;
            const auto a = autoGainStep(st, obs, kDefault);
            if (a.warnFloorOnce) ++warnings;
            st = a.next;
        }
        check(warnings == 1, "exactly once in 100 s at the floor, not once per window");
    }

    // ---- 11. THE WARNING STAYS LIT WHILE GAIN IS HELD DOWN ----------------
    //
    // The operator is not told "clear" while the loop is still holding gain
    // down: from their side those two states look identical and are not.
    {
        AutoGainState st;
        st.offsetDb = 9;
        st.warmupRemaining = 0;
        AutoGainObservation obs;
        obs.samples = 19;
        obs.overloadSamples = 0;      // perfectly clean, BECAUSE of the offset
        obs.elapsedMs = 100;
        const auto a = autoGainStep(st, obs, kDefault);
        check(a.holdWarning,
              "a clean window with an offset still applied keeps the warning lit");
        AutoGainState zero;
        zero.warmupRemaining = 0;
        check(!autoGainStep(zero, obs, kDefault).holdWarning,
              "and a clean window at zero offset clears it");
    }

    // ---- 12. A SHRINKING CEILING IS GIVEN BACK AT ONCE --------------------
    //
    // The operator lowering their baseline, or pulling in the floor control,
    // moves the ceiling under a held offset. That is a bound being enforced,
    // not the loop deciding to release, so it does not wait out a dwell.
    {
        AutoGainState st;
        st.offsetDb = 20;
        st.warmupRemaining = 0;
        AutoGainObservation obs;
        obs.samples = 19;
        obs.overloadSamples = 0;
        obs.elapsedMs = 100;
        obs.availableOffsetDb = 6;
        const auto a = autoGainStep(st, obs, kDefault);
        check(a.next.offsetDb == 6 && a.deltaDb == -14,
              "an offset above a newly-lowered ceiling is surrendered immediately, "
              "in one step, without a dwell");
    }

    // ---- 13. A BAND CHANGE FORGETS THAT BAND'S WALL -----------------------
    {
        AutoGainState st;
        st.offsetDb = 12;
        st.tripOffsetDb = 10;
        st.tripMarginDb = 2;
        st.warmupRemaining = 0;
        st.cleanMs = 100000;
        AutoGainObservation obs;
        obs.samples = 19;
        obs.overloadSamples = 0;
        obs.elapsedMs = 100;
        obs.bandChanged = true;
        const auto a = autoGainStep(st, obs, kDefault);
        check(a.next.tripOffsetDb < 0,
              "a new band starts with no memory of where the old one clipped");
        obs.bandChanged = false;
        obs.baselineMoved = true;
        const auto b = autoGainStep(st, obs, kDefault);
        check(b.next.tripOffsetDb < 0,
              "and the operator moving their own baseline supersedes it too");
    }

    // =====================================================================
    // PROBING RELEASE (#5535). The clip flag says "too high" and nothing says
    // "there is room", so a release is a PROBE and it can fail. Everything
    // below is about what a failed probe costs and how the loop stops paying.
    // =====================================================================

    const AutoGainConfig kProbe = probingReleaseConfig();

    // ---- 14. THE DEFAULTS ARE UNTOUCHED -----------------------------------
    //
    // Probing is opt-in through two config fields, both defaulting to the
    // behaviour that shipped. If either default moves, every other property in
    // this file is testing a different controller than the one the backend runs
    // with no configuration.
    {
        check(kDefault.probeConfirmMs == 0,
              "probing is off by default: a release is never confirmed");
        check(kDefault.tripFloorBindsRelease,
              "and the per-band trip memory still binds the release floor by "
              "default");
    }

    // ---- 15. THE CONSTANTS ARE THE MEASURED ONES --------------------------
    {
        check(kProbe.attackStepDb == 6 && kProbe.releaseStepDb == 6
                  && kProbe.firstStepHotDb == 6 && kProbe.firstStepMarginalDb == 6,
              "one 6 dB quantum in both directions - the measured knee is 3-5 dB "
              "wide, so one step clears it and cannot stall inside it");
        check(kProbe.maxOffsetDb % kProbe.attackStepDb == 0,
              "the ceiling is a whole number of steps, so no move is ever "
              "truncated to less than the knee width");
        check(kProbe.releaseDwellMs == 30000 && kProbe.dwellBackoffMaxMs == 480000,
              "base probe interval 30 s, cap 8 min - four doublings apart");
        check(kProbe.probeConfirmMs == 3000 && kProbe.releaseIntervalMs == 3000,
              "a probe is believed after 3 s, and the next one follows at once");
        check(kProbe.tripBackoffWindowMs > kProbe.dwellBackoffMaxMs,
              "a failed probe taken AT the cap still counts as a repeat, or the "
              "backoff would stop compounding exactly where it matters");
        check(kProbe.tripForgetMs > kProbe.dwellBackoffMaxMs,
              "and nothing forgets the trip before the backoff has run, because "
              "forgetting resets the interval");
    }

    // ---- 16. A FAILED PROBE COSTS ONE DETECTION WINDOW --------------------
    //
    // THE NUMBER THE WHOLE SCHEME RESTS ON. The step goes on at the end of one
    // window, the clip is observed across the next, and the step comes back off
    // at its end. Not two windows, not a dwell.
    {
        DiurnalPlant hot;
        hot.eveningClipBelowOffsetDb = 6;   // needs exactly one step, forever
        const ProbeRun r = probeRun(kProbe, hot, ProbeDrive{4000, 100, 24});
        check(r.probes >= 3, "the loop probes repeatedly on a permanently hot band");
        check(r.failedProbes == r.probes,
              "and on a band that has not improved, every probe fails");
        check(r.windowsProbeInFlight == r.failedProbes,
              "each failed probe rails the converter for EXACTLY ONE detection "
              "window - 100 ms at MetisClient::kTelemetryMinIntervalMs");
        std::printf("       [obs] %d probes, %d clipped windows in flight, "
                    "%d clipped windows total over 400 s\n",
                    r.probes, r.windowsProbeInFlight, r.clippedWindows);
    }

    // ---- 17. A FAILED PROBE IS UNDONE EXACTLY --------------------------------
    {
        AutoGainState st;
        st.offsetDb = 6;
        st.tripOffsetDb = 0;
        st.warmupRemaining = 0;
        st.cleanMs = 40000;          // past the base probe interval
        st.sinceReleaseMs = 10000;
        st.sinceAttackMs = 40000;
        AutoGainObservation obs;
        obs.samples = 19;
        obs.overloadSamples = 0;
        obs.elapsedMs = 100;
        obs.availableOffsetDb = 24;
        const auto probe = autoGainStep(st, obs, kProbe);
        check(probe.deltaDb == -6 && probe.next.offsetDb == 0
                  && probe.reason == AutoGainReason::Release,
              "the probe removes exactly one 6 dB step");
        check(probe.next.releasedSinceTrip,
              "and marks itself in flight, so a clip now is a FAILED PROBE and "
              "not an unrelated trip");
        AutoGainObservation clip = obs;
        clip.overloadSamples = 19;
        const auto back = autoGainStep(probe.next, clip, kProbe);
        check(back.deltaDb == 6 && back.next.offsetDb == 6,
              "and one window later the same 6 dB is back, exactly");
    }

    // ---- 18. A FAILED PROBE DOUBLES THE INTERVAL, UP TO THE CAP -----------
    {
        DiurnalPlant hot;
        hot.eveningClipBelowOffsetDb = 6;
        const ProbeRun r = probeRun(kProbe, hot, ProbeDrive{30000, 100, 24});
        const std::vector<std::int64_t>& iv = r.intervalAfterFailure;
        bool ladder = iv.size() >= 5;
        const std::int64_t want[5] = {60000, 120000, 240000, 480000, 480000};
        for (std::size_t k = 0; ladder && k < 5; ++k) {
            if (iv[k] != want[k]) ladder = false;
        }
        std::printf("       [obs] probe intervals after each failure:");
        for (std::size_t k = 0; k < iv.size() && k < 8; ++k) {
            std::printf(" %lld", static_cast<long long>(iv[k]));
        }
        std::printf(" ms\n");
        check(ladder,
              "each failed probe doubles the interval - 60, 120, 240, 480 s - "
              "and then holds at the 8 min cap");
        bool monotone = true;
        for (std::size_t k = 1; k < iv.size(); ++k) {
            if (iv[k] < iv[k - 1]) monotone = false;
        }
        check(monotone, "a backoff never SHORTENS the interval");
    }

    // ---- 19. THE COST SELF-LIMITS ----------------------------------------
    //
    // The asymmetry, as a number rather than an adjective: on a band that never
    // improves, the deliberate clipping the loop causes gets rarer with time.
    {
        DiurnalPlant hot;
        hot.eveningClipBelowOffsetDb = 6;
        ProbeDrive early{6000, 100, 24};        // first 600 s
        ProbeDrive late{36000, 100, 24};
        late.countClipsFrom = 30000;            // the 3000 s - 3600 s slice
        const ProbeRun a = probeRun(kProbe, hot, early);
        const ProbeRun b = probeRun(kProbe, hot, late);
        std::printf("       [obs] deliberate clips: %d in the first 600 s, "
                    "%d in the 600 s an hour later\n",
                    a.windowsProbeInFlight, b.windowsClippedAfter);
        check(b.windowsClippedAfter < a.windowsProbeInFlight,
              "an hour into a hot band the loop probes less often than it did in "
              "the first ten minutes");
        check(b.windowsClippedAfter <= 2,
              "and at the cap it costs at most a couple of 100 ms windows per "
              "ten minutes");
    }

    // ---- 20. A CONFIRMED PROBE RESETS THE INTERVAL TO BASE ----------------
    //
    // Without this the loop carries last night's backoff into this afternoon:
    // the interval reached the cap while the band was hot and nothing ever puts
    // it back, so the first failure of the NEW day starts from 8 min.
    {
        AutoGainState st;
        st.offsetDb = 6;
        st.tripOffsetDb = 0;
        st.dwellRequiredMs = 480000;     // the cap, reached overnight
        st.releasedSinceTrip = false;
        st.warmupRemaining = 0;
        DiurnalPlant quiet;
        quiet.eveningClipBelowOffsetDb = 0;   // the band has gone quiet
        const ProbeRun r = probeRun(kProbe, quiet, ProbeDrive{6000, 100, 24}, st);
        std::printf("       [obs] after a confirmed probe: dwellRequiredMs=%lld, "
                    "releasedSinceTrip=%d, offset=%d\n",
                    static_cast<long long>(r.state.dwellRequiredMs),
                    r.state.releasedSinceTrip ? 1 : 0, r.state.offsetDb);
        check(r.state.dwellRequiredMs == 0,
              "a probe that stays clean for the confirmation period resets the "
              "interval to base");
        check(!r.state.releasedSinceTrip,
              "and is no longer in flight, so a clip an hour later is a FRESH "
              "trip paced from base, not a continuation of last night's backoff");
        check(r.state.offsetDb == 0,
              "and the gain it reclaimed stays reclaimed");
    }

    // ---- 21. THE REMEMBERED OFFSET MUST NOT FLOOR THE RELEASE -------------
    //
    // The diurnal objection, as a test. A loop that remembers where it clipped
    // in DECIBELS is a lookup table, and a knee that moves 18 dB destroys it.
    {
        DiurnalPlant diurnal;
        diurnal.eveningClipBelowOffsetDb = 18;
        diurnal.daytimeClipBelowOffsetDb = 0;
        diurnal.dawnWindow = 20000;               // 2000 s in
        ProbeDrive drive{40000, 100, 24};
        drive.countClipsFrom = 20000;

        AutoGainConfig floored = kProbe;
        floored.tripFloorBindsRelease = true;     // the original memory
        const ProbeRun stuck = probeRun(floored, diurnal, drive);
        const ProbeRun free = probeRun(kProbe, diurnal, drive);
        std::printf("       [obs] at dawn+2000 s: floor-bound offset %d dB, "
                    "probing offset %d dB\n",
                    stuck.state.offsetDb, free.state.offsetDb);
        check(stuck.state.offsetDb > 0,
              "with the trip memory binding the floor the loop is STRANDED "
              "attenuated long after the band went quiet");
        check(free.state.offsetDb == 0,
              "probing walks the whole way back, because its memory is the "
              "widening interval and not a remembered decibel");
        check(free.firstZeroOffsetWindow >= 0
                  && free.firstZeroOffsetWindow - drive.countClipsFrom < 6000,
              "and it gets there inside ten minutes of the band going quiet - "
              "one capped interval to the first confirmed probe, then 6 dB per "
              "confirmation period");
    }

    // ---- 22. THE RECLAIM IS THE FAST HALF --------------------------------
    {
        AutoGainState st;
        st.offsetDb = 24;
        st.tripOffsetDb = 18;
        st.warmupRemaining = 0;
        st.cleanMs = 40000;
        st.sinceAttackMs = 40000;
        st.sinceReleaseMs = 40000;
        DiurnalPlant quiet;
        quiet.eveningClipBelowOffsetDb = 0;
        const ProbeRun r = probeRun(kProbe, quiet, ProbeDrive{400, 100, 24}, st);
        const int reclaimWindows = r.firstZeroOffsetWindow - r.firstProbeWindow;
        std::printf("       [obs] first probe at window %d (%d ms after arming); "
                    "24 dB reclaimed in %d windows (%d ms) from there\n",
                    r.firstProbeWindow, r.firstProbeWindow * 100,
                    reclaimWindows, reclaimWindows * 100);
        // ARMING DOES NOT RECLAIM EAGERLY. `resetWarmup` clears `cleanMs`, so
        // even a state that arrives holding 24 dB with a long clean history
        // must earn a whole base probe interval of fresh observation before it
        // asks for more gain. That is the timid half and it is deliberate.
        check(r.firstProbeWindow >= 300,
              "the FIRST probe after arming waits a whole base interval - a "
              "loop that reclaimed on arrival would be reclaiming on evidence "
              "it gathered under a denominator that has since changed");
        check(reclaimWindows > 0 && reclaimWindows <= 130,
              "but from that first probe the whole 24 dB comes back in about "
              "twelve seconds - one 6 dB step per confirmation period");
        check(r.clippedWindows == 0,
              "and a reclaim into a genuinely quiet band costs no clipping at all");
    }

    // ---- 23. TRANSMIT DOES NOT PAY FOR THE PROBE -------------------------
    //
    // `cleanMs` is reset by keying and by the 300 ms post-unkey hold-off, so a
    // probe cannot fire until the loop has had a whole uninterrupted probe
    // interval of receive. THIS IS THE HONEST LIMIT: a probe cannot land in a
    // short over, and it CAN land inside a long one.
    {
        DiurnalPlant hot;
        hot.eveningClipBelowOffsetDb = 6;
        ProbeDrive shortOvers{20000, 100, 24};
        shortOvers.keyedPeriodWindows = 230;   // 20 s receive, 3 s transmit
        shortOvers.keyedForWindows = 30;
        const ProbeRun a = probeRun(kProbe, hot, shortOvers);
        check(a.probes == 0,
              "overs shorter than the probe interval never produce a probe - the "
              "clean accumulator is reset by every unkey");

        ProbeDrive longOvers{20000, 100, 24};
        longOvers.keyedPeriodWindows = 630;    // 60 s receive, 3 s transmit
        longOvers.keyedForWindows = 30;
        const ProbeRun b = probeRun(kProbe, hot, longOvers);
        std::printf("       [obs] probes in 2000 s: %d with 20 s overs, "
                    "%d with 60 s overs\n", a.probes, b.probes);
        check(b.probes > 0,
              "an over longer than the probe interval CAN carry a deliberate "
              "clip, and this test is where that is admitted");
    }

    // ---- 24. NOTHING PROBES INSIDE THE POST-UNKEY HOLD-OFF ---------------
    {
        bool probedInHoldoff = false;
        for (std::int64_t sinceUnkey = 0; sinceUnkey < 600; sinceUnkey += 7) {
            AutoGainState st;
            st.offsetDb = 12;
            st.tripOffsetDb = 0;
            st.warmupRemaining = 0;
            st.cleanMs = 600000;
            st.sinceReleaseMs = 600000;
            AutoGainObservation obs;
            obs.samples = 19;
            obs.overloadSamples = 0;
            obs.elapsedMs = 100;
            obs.availableOffsetDb = 24;
            obs.msSinceUnkey = sinceUnkey;
            const auto a = autoGainStep(st, obs, kProbe);
            if (sinceUnkey < kProbe.unkeyHoldoffMs && a.deltaDb != 0) {
                probedInHoldoff = true;
            }
        }
        check(!probedInHoldoff,
              "no probe is issued inside the measured 300 ms post-unkey window, "
              "where the receive path is still describing our own transmitter");
    }

    // ---- 25. A BAND CHANGE TAKES THE PROBE INTERVAL WITH IT ---------------
    {
        AutoGainState st;
        st.offsetDb = 12;
        st.tripOffsetDb = 6;
        st.dwellRequiredMs = 480000;
        st.releasedSinceTrip = true;
        st.warmupRemaining = 0;
        AutoGainObservation obs;
        obs.samples = 19;
        obs.overloadSamples = 0;
        obs.elapsedMs = 100;
        obs.availableOffsetDb = 24;
        obs.bandChanged = true;
        const auto a = autoGainStep(st, obs, kProbe);
        check(a.next.dwellRequiredMs == 0 && a.next.tripOffsetDb < 0
                  && !a.next.releasedSinceTrip,
              "a new band starts probing from base - the backoff described the "
              "band we left");
        obs.bandChanged = false;
        obs.baselineMoved = true;
        const auto b = autoGainStep(st, obs, kProbe);
        check(b.next.dwellRequiredMs == 0 && !b.next.releasedSinceTrip,
              "and so does the operator moving their own baseline");
    }

    // ---- 26. SAFETY STILL HOLDS UNDER PROBING -----------------------------
    {
        bool everOut = false;
        for (int x = 0; x <= 30; ++x) {
            DiurnalPlant p;
            p.eveningClipBelowOffsetDb = x;
            const ProbeRun r = probeRun(kProbe, p, ProbeDrive{3000, 100, 24});
            for (const int o : r.offsetTrace) {
                if (o < 0 || o > 24) everOut = true;
            }
        }
        check(!everOut,
              "over 31 plants x 300 s of probing the offset never leaves "
              "[0, ceiling]");
    }

    // ---- PROPERTY 8 · THE MEASURED-HEADROOM RELEASE -----------------------
    //
    // The bandscope turns a release from a probe into a decision. What must be
    // true of that, and every one of these is otherwise reachable only by
    // pointing a real antenna at a real broadcast station:
    //
    //   * the loop behaves EXACTLY as it did before when the sensor is not
    //     required -- the feature costs nothing when it is off;
    //   * no reading holds the loop, and holds it DIFFERENTLY from a reading
    //     that says "not enough";
    //   * the licence is checked against the step actually being taken;
    //   * a refusal is not a failed probe and must not earn a backoff.
    {
        auto measured = [](double db) {
            HeadroomObservation h;
            h.state = BandscopeHeadroom::Measured;
            h.headroomDb = db;
            return h;
        };
        auto railed = [] {
            HeadroomObservation h;
            h.state = BandscopeHeadroom::AtRail;
            return h;
        };

        const double bias = gatedPeakBiasDbForPeriod(1000);   // 3.77 dB
        const AutoGainConfig kBandscope = bandscopeReleaseConfig(bias);

        check(kBandscope.requireHeadroomToRelease,
              "8.0 bandscopeReleaseConfig requires a measured headroom");
        check(kBandscope.headroomBiasDb > 3.7 && kBandscope.headroomBiasDb < 3.8,
              "8.0 it budgets the gate's 3.77 dB sampling bias");
        check(kBandscope.headroomRailAttacks,
              "8.0 a railed block may escalate a clean window");

        // A loop holding 12 dB, clean for far longer than the dwell, and past
        // its release interval. Everything the clip flag can say about a
        // release has already been said; only the measurement is left.
        auto readyToRelease = [](int offsetDb) {
            AutoGainState st;
            st.offsetDb = offsetDb;
            st.cleanMs = 600000;
            st.sinceReleaseMs = 600000;
            st.sinceAttackMs = 600000;
            return st;
        };
        AutoGainObservation clean;
        clean.samples = 19;
        clean.overloadSamples = 0;
        clean.elapsedMs = 100;

        // -- the requirement is real, in both directions --
        {
            AutoGainObservation o = clean;
            o.headroom = measured(12.0);        // 12 >= 6 + 3.77 + 2 = 11.77
            const AutoGainAction a =
                autoGainStep(readyToRelease(12), o, kBandscope);
            check(a.reason == AutoGainReason::Release && a.deltaDb == -6,
                  "8.1 twelve dB of measured room releases the full 6 dB step");

            o.headroom = measured(11.0);        // one dB short of the same sum
            const AutoGainAction b =
                autoGainStep(readyToRelease(12), o, kBandscope);
            check(b.reason == AutoGainReason::HeadroomHold && b.deltaDb == 0,
                  "8.2 one dB short of the requirement holds, and says WHY");
            check(b.next.offsetDb == 12,
                  "8.2 a refused release does not move the offset");
        }

        // -- absent is not zero and is not infinity --
        {
            AutoGainObservation o = clean;      // o.headroom defaults to Absent
            const AutoGainAction a =
                autoGainStep(readyToRelease(12), o, kBandscope);
            check(a.reason == AutoGainReason::HeadroomAbsent && a.deltaDb == 0,
                  "8.3 no reading at all holds the loop, with its own reason");
            check(a.reason != AutoGainReason::HeadroomHold,
                  "8.3 'no reading' is reported differently from 'not enough'");
            // And it does NOT silently degrade into blind probing: a loop that
            // fell back to releasing on the clip flag alone would have taken
            // the step here, which is exactly what an operator who chose a
            // measured mode did not ask for.
            check(a.next.offsetDb == 12,
                  "8.3 an absent reading never licenses a blind release");
        }

        // -- the licence is about the step ACTUALLY being taken --
        //
        // Holding 2 dB, the step truncates from 6 to 2. A reading that could
        // not license 6 dB can license 2, and a law that compared against
        // cfg.releaseStepDb would refuse a release that fits.
        {
            AutoGainObservation o = clean;
            o.headroom = measured(8.0);         // 8 >= 2 + 3.77 + 2 = 7.77
            const AutoGainAction a =
                autoGainStep(readyToRelease(2), o, kBandscope);
            check(a.reason == AutoGainReason::Release && a.deltaDb == -2,
                  "8.4 the truncated final step is what the reading must cover");
            // The same reading against a full-width step is refused, which is
            // what makes the check above about delta rather than a constant.
            const AutoGainAction b =
                autoGainStep(readyToRelease(12), o, kBandscope);
            check(b.reason == AutoGainReason::HeadroomHold,
                  "8.4 the same 8 dB does not license a full 6 dB step");
        }

        // -- a refusal is not a failed probe --
        //
        // A failed probe doubles the interval that paces the next one. A
        // refusal is the loop declining to move at all, so there is nothing for
        // the band to have punished and the backoff must not advance.
        {
            AutoGainObservation o = clean;
            o.headroom = measured(1.0);
            AutoGainState st = readyToRelease(12);
            const AutoGainAction a = autoGainStep(st, o, kBandscope);
            check(a.reason == AutoGainReason::HeadroomHold,
                  "8.5 the fixture really is a refusal");
            check(!a.next.releasedSinceTrip,
                  "8.5 a refused release does not count as having released");
            check(a.next.dwellRequiredMs == st.dwellRequiredMs,
                  "8.5 a refusal does not widen the probe interval");
        }

        // -- a railed block is a clip, and only ever escalates --
        {
            AutoGainObservation o = clean;      // the clip bit says Clean
            o.headroom = railed();
            AutoGainState st;
            st.sinceAttackMs = 600000;          // past the quiet period
            const AutoGainAction a = autoGainStep(st, o, kBandscope);
            check(a.reason == AutoGainReason::AttackMarginal && a.deltaDb > 0,
                  "8.6 a railed bandscope block attacks even when the clip bit "
                  "window read clean");

            // It does not rescue a Void window: too few observations still
            // means the loop is not being fed, and holding is the safe rule.
            AutoGainObservation v = o;
            v.samples = 0;
            v.overloadSamples = 0;
            const AutoGainAction b = autoGainStep(st, v, kBandscope);
            check(b.reason == AutoGainReason::Void || b.reason == AutoGainReason::Stale,
                  "8.6 a railed block does not make a Void window actionable");

            // And it cannot soften a Hot one.
            AutoGainObservation h = o;
            h.overloadSamples = 19;
            const AutoGainAction c = autoGainStep(st, h, kBandscope);
            check(c.reason == AutoGainReason::AttackHot,
                  "8.6 a railed block never downgrades a Hot window");
        }

        // -- THE EQUIVALENCE, and it is what makes this safe to ship off --
        //
        // With the requirement off, the headroom field is inert: the same
        // inputs give the same action whatever the bandscope says, including
        // when it says the converter is at the rail. A loop running "ramp",
        // "probe" or "binary" is therefore exactly the loop that existed before
        // this sensor did.
        {
            const AutoGainConfig kProbeOnly = probingReleaseConfig();
            check(!kProbeOnly.requireHeadroomToRelease && !kProbeOnly.headroomRailAttacks,
                  "8.7 the pre-existing laws do not use the sensor");
            bool allSame = true;
            for (int offset = 0; offset <= 24; offset += 6) {
                for (int over = 0; over <= 19; over += 19) {
                    AutoGainObservation bare;
                    bare.samples = 19;
                    bare.overloadSamples = over;
                    bare.elapsedMs = 100;
                    AutoGainObservation withRail = bare;
                    withRail.headroom = railed();
                    AutoGainObservation withRoom = bare;
                    withRoom.headroom = measured(40.0);
                    const AutoGainState st = readyToRelease(offset);
                    const AutoGainAction x = autoGainStep(st, bare, kProbeOnly);
                    const AutoGainAction y = autoGainStep(st, withRail, kProbeOnly);
                    const AutoGainAction z = autoGainStep(st, withRoom, kProbeOnly);
                    if (x.deltaDb != y.deltaDb || x.reason != y.reason
                        || x.deltaDb != z.deltaDb || x.reason != z.reason
                        || x.next.offsetDb != y.next.offsetDb
                        || x.next.offsetDb != z.next.offsetDb) {
                        allSame = false;
                    }
                }
            }
            check(allSame,
                  "8.7 with the requirement off the headroom reading changes "
                  "nothing, at any offset and either window verdict");
        }
    }

    // WHAT NO TEST HERE SUPPLIES: the plant. Property 7 tests the controller
    // against a model of the radio, and nothing in it says the model is the
    // radio. Specifically absent — the true observation rate at each sample
    // rate and receiver count with the application issuing commands; whether
    // the clip rate is monotone in gain with usable resolution across the six
    // decibels that span the whole transition; whether 3 dB steps are audible
    // as steps; and whether the loop pumps. Those need an antenna.

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
