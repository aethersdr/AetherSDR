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
using AetherSDR::hl2::classifyWindow;

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
