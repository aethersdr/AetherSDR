#pragma once

// The automatic receive-gain control law, as a pure function.
//
//     (state, observation, config) -> action
//
// No Qt, no socket, no clock, no radio. Elapsed time is an INPUT and the
// function returns an INSTRUCTION about state, exactly as `adcOverloadWarn`
// returns `restartClock` rather than touching a `QElapsedTimer`. `Hl2Backend`
// owns the timers and the register write; this header owns every decision.
//
// It lives in a header, evaluated by the backend rather than copied into it,
// for the reason `Hl2TxLevelPolicy.h` states: a test against a re-typed copy of
// a decision proves only that two copies agree.
//
// ---------------------------------------------------------------------------
// WHY THIS IS PARAMETERISED RATHER THAN A FIXED SERVO
// ---------------------------------------------------------------------------
//
// The obvious shape for this feature is a servo: small steps, a dwell, a slow
// release. There is a measurement that says the obvious shape may be wrong.
//
// A sweep on ON8ST's station (issue #5354's own table) gives, at 14.2 MHz:
//
//     LNA -12 dB -> 0 % clipping        LNA  +0 dB -> 86 % clipping
//     LNA  -6 dB -> 0 % clipping        LNA  +6 dB -> 100 % clipping
//
// SIX DECIBELS SPANS THE ENTIRE 0 % -> 86 % TRANSITION. The smallest attack
// step worth taking is 3 dB — half of that. A controller whose step is half its
// plant's whole linear region is not a servo; it is a two-state switch wearing
// a servo's machinery, and on that evidence a per-band binary high-gain /
// low-gain decision with a long hold may simply be the better feature.
//
// The measurement that would settle it has not been made — nobody has yet
// watched this observable on a live antenna, because it turns out to be a
// single bit that only updates while the radio is streaming. So this header
// declines to choose. Step size, thresholds, dwell, cooldown and the trip
// memory are all configuration, and `binaryHighLowConfig()` below is the binary
// controller expressed as one particular `AutoGainConfig` — the same function,
// the same state machine, the same tests, different numbers. The bench decides
// by measurement, and neither outcome needs a rewrite.
//
// ---------------------------------------------------------------------------
// WHAT THE OBSERVATION ACTUALLY IS, AND WHAT THAT FORCES
// ---------------------------------------------------------------------------
//
// The Hermes-Lite 2's "2-bit saturating clip counter" carries ONE bit, and it
// is the same bit as the ADC overload flag: response address 0's DATA[24] is
// `(&clip_cnt)`, the reduction AND of the counter, and against a clear interval
// of order a millisecond the counter is binary to within a fraction of a
// percent of windows. Reading the counter buys nothing over reading the bit.
//
// Two consequences are load-bearing here and are behaviour, not commentary:
//
//   1. THE OBSERVATION ONLY EXISTS WHILE STREAMING. The counter's only clear is
//      the EP6 response cycle, which runs only inside the streaming datagram
//      path; its increment is ungated. At idle it is a LATCH, not a level. So
//      when the evidence stops arriving the loop HOLDS its offset — it does not
//      decay, does not release, and does not advance the release dwell. A
//      window with too few observations is `Void`, and `Void` IS NOT `Clean`:
//      releasing gain into a stalled stream is the one failure a loop built on
//      a bare overload bit is most likely to have.
//
//   2. THE RATE IS THE ONLY GRADED THING AVAILABLE. A single bit sampled ~190
//      times a second, accumulated as a numerator over a denominator across a
//      window, is what lets the first attack step be sized. It is not a
//      magnitude and must not be treated as one, which is why the classifier
//      below quantises to three states rather than using the proportion
//      directly: the crossing that produces the bit can MISS assertions, so an
//      observed rate is a LOWER BOUND on the true one. Three coarse states are
//      robust to that; a proportional law would not be.
//
// The denominator is not ours to assume, either: command responses displace the
// slot that carries this bit, at up to half of them, and the application issues
// commands in response to operator activity. So a count of adverse events
// arrives with its denominator or it is not usable — `minSamples` is that rule
// as a gate rather than as a reporting convention.
//
// ---------------------------------------------------------------------------
// THE AXIS
// ---------------------------------------------------------------------------
//
// `offsetDb` is a NON-NEGATIVE attenuation below the operator's baseline (see
// Hl2GainSplit.h). This law has no way to express a gain above the number the
// operator set, so it cannot make the radio louder than they asked and cannot
// reach the AD9866 register region above +19 dB unless they are already in it.
// The only automatic action in the loud direction is undoing one of its own.

#include <cstdint>

namespace AetherSDR::hl2 {

// ---- The observation, quantised ------------------------------------------

enum class AutoGainWindow {
    Void,      // too few observations to say anything. NOT Clean.
    Clean,     // observations arrived and none carried the overload bit
    Marginal,  // the converter railed in some of them
    Hot        // it railed in more of them than not
};

enum class AutoGainReason {
    Disarmed,      // nothing armed this loop
    Warmup,        // a transition just happened; nothing acts on the first evidence
    Keyed,         // the radio hears its own transmitter; every reading is a lie
    UnkeyHoldoff,  // still inside the measured post-unkey settling window
    Void,          // not enough evidence this window
    Stale,         // no valid window for long enough that the evidence is gone
    Cooldown,      // an attack is due but the shared cooldown has not expired
    AttackHot,
    AttackMarginal,
    AtFloor,       // out of range and still railing
    Dwell,         // clean, but not for long enough to start releasing
    ReleaseHold,   // clean and dwelt, but the band's trip memory says no further
    Release,
    Idle           // clean, and there is no offset to give back
};

struct AutoGainConfig {
    // ---- the observation gate ----
    // Below this many observations in a window, the window is Void whatever the
    // numerator says. A count of adverse events without its denominator is not
    // evidence; this is that rule as a gate.
    int minSamples = 4;
    // Hot when overloadSamples * hotDenominator > samples * hotNumerator, i.e.
    // "railed in more windows than not" at the 1/2 default.
    int hotNumerator = 1;
    int hotDenominator = 2;

    // ---- attack ----
    // The first step out of a quiet period is sized by the RATE, which is the
    // one thing this observation has that a bare event does not.
    int firstStepHotDb = 6;
    int firstStepMarginalDb = 3;
    int attackStepDb = 3;
    // How long without an attack counts as "a quiet period" for step sizing.
    std::int64_t quietPeriodMs = 3000;
    // ONE cooldown, shared by the first-observation step and the ramp, advanced
    // only when the offset ACTUALLY MOVED. Without it, alternating hot/clean
    // across window boundaries re-enters "first observation, act now" over and
    // over and rails the offset in a few hundred milliseconds. This is the
    // single most important constant here.
    std::int64_t attackCooldownMs = 200;

    // ---- release, which is the timid direction ----
    int releaseStepDb = 1;
    std::int64_t releaseIntervalMs = 500;
    std::int64_t releaseDwellMs = 3000;

    // ---- range ----
    // How deaf the loop may make the receiver. The operator owns this number
    // and the on/off switch; nothing else here is theirs to set.
    int maxOffsetDb = 26;

    // ---- guards ----
    // MEASURED BOUND, not borrowed. This lab's run `d83-unkey-transient`
    // (FINDINGS.md FIND-16) measured the receive path still describing the
    // operator's own transmission 178-285 ms after unkey, median 229, across
    // ten windows, and states that a hold covering it "would have to run past
    // ~300 ms". 300 is that bound, rounded up to it.
    std::int64_t unkeyHoldoffMs = 300;
    // No valid window for this long: the offset FREEZES and the state is
    // reported stale. A dead overload bit is not a clean converter.
    std::int64_t stalenessMs = 1000;
    // Consecutive valid windows discarded after connect, band change, sample-
    // rate or receiver-count change, and unkey. Nothing acts on the first
    // evidence after a transition, because the denominator just changed.
    int warmupWindows = 3;
    // At the floor and still railing for this long: stop, keep the warning lit
    // and say ONCE that the front end needs attenuation ahead of the radio.
    // The stranded-deaf failure, surfaced rather than silently endured.
    std::int64_t floorAlarmMs = 10000;

    // ---- the per-band trip memory ----
    //
    // Without this the loop re-probes into a known wall forever: release 1 dB
    // at a time until it clips, attack, dwell, repeat, with a period of a few
    // seconds and no end. Remembering where it clipped converts a perpetual
    // hunt into one that converges in a handful of trips and then stops.
    //
    // Set tripMarginDb and tripMarginGrowthDb to 0 to disable the memory, which
    // is what the binary configuration does.
    int tripMarginDb = 2;
    int tripMarginGrowthDb = 1;
    int tripMarginMaxDb = 4;
    // Propagation changes. A loop that never forgets never recovers a band that
    // has gone quiet.
    std::int64_t tripForgetMs = 300000;
    // A second trip within this window is a repeat, and widens the backoff.
    std::int64_t tripBackoffWindowMs = 60000;
    // Each repeat doubles the release dwell, capped.
    std::int64_t dwellBackoffMaxMs = 30000;
};

// The binary per-band high-gain / low-gain controller, as a configuration of
// the same law. One step takes the whole range in each direction, so the offset
// only ever holds one of two values; the long hold is what stops it dithering.
//
// This exists to make the choice a measurement rather than an architecture
// decision. If the bench shows the plant's transition really is abrupt enough
// that a ramp is meaningless, this config is the answer and nothing else has to
// change: same function, same state machine, same tests.
[[nodiscard]] constexpr AutoGainConfig binaryHighLowConfig(
    int lowOffsetDb = 26, std::int64_t holdMs = 60000) noexcept
{
    AutoGainConfig c;
    c.maxOffsetDb = lowOffsetDb;
    // One step, either way, is the whole range.
    c.firstStepHotDb = lowOffsetDb;
    c.firstStepMarginalDb = lowOffsetDb;
    c.attackStepDb = lowOffsetDb;
    c.releaseStepDb = lowOffsetDb;
    // The hold IS the hysteresis in a two-state controller; there is no ramp to
    // pace, so the release interval collapses into the dwell.
    c.releaseDwellMs = holdMs;
    c.releaseIntervalMs = 0;
    // No trip memory: a two-state controller's whole job is to be able to go
    // back to the high state and find out, and a margin would strand it low.
    c.tripMarginDb = 0;
    c.tripMarginGrowthDb = 0;
    c.tripMarginMaxDb = 0;
    // The hold is the whole mechanism, so the backoff cap has to be at least
    // the hold or a repeat trip would SHORTEN it.
    c.dwellBackoffMaxMs = holdMs;
    return c;
}

// Every accumulated interval saturates here rather than overflowing: a
// session left running for a month is not an arithmetic problem.
inline constexpr std::int64_t kElapsedCapMs = 1'000'000'000;

struct AutoGainState {
    int offsetDb = 0;
    // The HIGHEST offset at which this band has been seen to clip, or < 0 for
    // "no trip recorded". Highest, not lowest: the binding constraint is the
    // DEEPEST attenuation that still railed, because that is the one that says
    // where the loop must not return to. Remembering the shallowest instead
    // would record a fact the loop already knew and would let it hunt forever.
    int tripOffsetDb = -1;
    int tripMarginDb = 0;      // grows with repeat trips, capped
    // TRUE once the loop has given gain back since the last trip. What makes a
    // trip a REPEAT is that the loop released and the band clipped again — not
    // that the previous window of the same episode also clipped. Without this
    // the backoff fires on every window of one continuous overload and the
    // margin and dwell saturate in half a second, which is a different
    // controller from the one the widening backoff is meant to produce.
    bool releasedSinceTrip = false;
    std::int64_t dwellRequiredMs = 0;   // 0 = use the config's dwell
    std::int64_t cleanMs = 0;           // consecutive Clean time
    // "NO ATTACK HAS EVER HAPPENED" IS INFINITY, NOT ZERO. Starting these at 0
    // makes the very first threat after arming fail the quiet-period test and
    // take the ramp's small step instead of the rate-sized one — which is
    // exactly the moment the loop most needs to move, and the exactly wrong
    // moment to be timid. The saturating cap doubles as that sentinel.
    std::int64_t sinceAttackMs = kElapsedCapMs;
    std::int64_t sinceReleaseMs = kElapsedCapMs;
    std::int64_t sinceValidMs = 0;
    std::int64_t sinceTripMs = 0;
    std::int64_t atFloorHotMs = 0;
    int warmupRemaining = 0;
    bool stale = false;
    bool floorAlarmed = false;
};

struct AutoGainObservation {
    // Response-address-0 observations accumulated over this window, and how
    // many of them carried the overload bit. The denominator is not a constant:
    // it varies with sample rate, receiver count, and whether the application
    // is issuing commands.
    int samples = 0;
    int overloadSamples = 0;
    // Wall time this window covers. An INPUT: this function has no clock.
    std::int64_t elapsedMs = 0;
    // The radio hears its own transmitter at enormous strength, so nothing
    // observed while keyed describes the antenna.
    bool keyed = false;
    // < 0 means "not keyed since this loop was armed".
    std::int64_t msSinceUnkey = -1;
    // How much attenuation is physically available below the operator's
    // baseline before the AD9866's own floor (Hl2GainSplit.h computes it). The
    // policy does not know the register geometry and must not.
    int availableOffsetDb = 60;
    // Connect, band change, sample-rate change, receiver-count change. The
    // denominator changed, so the old windows are not comparable.
    bool resetWarmup = false;
    // The operator moved the baseline: their intent supersedes the band's trip
    // memory, which was recorded about a different starting point.
    bool baselineMoved = false;
    // A new band has its own memory.
    bool bandChanged = false;
};

struct AutoGainAction {
    // Signed change to apply to the offset this tick. Zero on every path that
    // is not an attack or a release.
    int deltaDb = 0;
    AutoGainState next;
    AutoGainReason reason = AutoGainReason::Idle;
    // Log ONCE: at the floor and still railing. The operator needs attenuation
    // ahead of the radio and no amount of LNA is going to supply it.
    bool warnFloorOnce = false;
    // The ADC-overload warning stays lit while any offset is held. The operator
    // is not told "clear" while the loop is still holding gain down, because
    // from their side those two states look identical and are not.
    bool holdWarning = false;
};

// ---- classification -------------------------------------------------------

[[nodiscard]] constexpr AutoGainWindow classifyWindow(int samples,
                                                      int overloadSamples,
                                                      const AutoGainConfig& cfg) noexcept
{
    if (samples < cfg.minSamples || samples <= 0) {
        return AutoGainWindow::Void;
    }
    const int over = overloadSamples < 0 ? 0
                   : (overloadSamples > samples ? samples : overloadSamples);
    if (over == 0) {
        return AutoGainWindow::Clean;
    }
    // Integer comparison rather than a ratio: no floating point, and no
    // rounding decision to disagree with a test about.
    if (static_cast<std::int64_t>(over) * cfg.hotDenominator
        > static_cast<std::int64_t>(samples) * cfg.hotNumerator) {
        return AutoGainWindow::Hot;
    }
    return AutoGainWindow::Marginal;
}

namespace detail {

constexpr std::int64_t addMs(std::int64_t a, std::int64_t b) noexcept
{
    const std::int64_t sum = a + (b < 0 ? 0 : b);
    return sum > kElapsedCapMs ? kElapsedCapMs : sum;
}

constexpr int clampInt(int lo, int v, int hi) noexcept
{
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace detail

// ---- the law --------------------------------------------------------------

[[nodiscard]] constexpr AutoGainAction autoGainStep(const AutoGainState& state,
                                                    const AutoGainObservation& obs,
                                                    const AutoGainConfig& cfg) noexcept
{
    AutoGainAction out;
    AutoGainState next = state;

    // Clocks advance on EVERY tick, including the ones that decide nothing.
    // The cooldown is wall time, not a count of decisions.
    next.sinceAttackMs = detail::addMs(next.sinceAttackMs, obs.elapsedMs);
    next.sinceReleaseMs = detail::addMs(next.sinceReleaseMs, obs.elapsedMs);
    if (next.tripOffsetDb >= 0) {
        next.sinceTripMs = detail::addMs(next.sinceTripMs, obs.elapsedMs);
    }

    // A band change hands the loop a different antenna problem. The offset
    // itself is kept — it describes the front end, not the band — but the
    // memory of where THAT band clipped does not transfer.
    if (obs.bandChanged) {
        next.tripOffsetDb = -1;
        next.tripMarginDb = 0;
        next.sinceTripMs = 0;
        next.dwellRequiredMs = 0;
        next.releasedSinceTrip = false;
    }
    // The operator moving their own baseline supersedes the loop's memory: the
    // trip was recorded relative to a starting point that no longer exists.
    if (obs.baselineMoved) {
        next.tripOffsetDb = -1;
        next.tripMarginDb = 0;
        next.sinceTripMs = 0;
        next.dwellRequiredMs = 0;
        next.releasedSinceTrip = false;
    }
    if (obs.resetWarmup) {
        next.warmupRemaining = cfg.warmupWindows;
        next.cleanMs = 0;
    }

    const int ceiling = detail::clampInt(0,
        cfg.maxOffsetDb < obs.availableOffsetDb ? cfg.maxOffsetDb : obs.availableOffsetDb,
        cfg.maxOffsetDb);
    // The offset can only be over the ceiling if the ceiling just moved under
    // it — the operator lowered their baseline, or the floor control was pulled
    // in. Give the excess back immediately rather than at the release rate:
    // this is not the loop deciding to release, it is a bound being enforced.
    if (next.offsetDb > ceiling) {
        out.deltaDb = ceiling - next.offsetDb;
        next.offsetDb = ceiling;
    }
    out.holdWarning = next.offsetDb > 0;

    // ---- transmit, before any observation is trusted ----
    //
    // The HL2 receives while it transmits and hears itself at enormous
    // strength, so the overload bit is slammed on every transmission. This is
    // not a rate limit; it is the difference between an observation and a lie.
    if (obs.keyed) {
        next.warmupRemaining = cfg.warmupWindows;
        next.cleanMs = 0;
        next.sinceValidMs = 0;
        out.next = next;
        out.reason = AutoGainReason::Keyed;
        return out;
    }
    if (obs.msSinceUnkey >= 0 && obs.msSinceUnkey < cfg.unkeyHoldoffMs) {
        next.warmupRemaining = cfg.warmupWindows;
        next.cleanMs = 0;
        next.sinceValidMs = 0;
        out.next = next;
        out.reason = AutoGainReason::UnkeyHoldoff;
        return out;
    }

    // ---- the window ----
    const AutoGainWindow w = classifyWindow(obs.samples, obs.overloadSamples, cfg);

    if (w == AutoGainWindow::Void) {
        next.sinceValidMs = detail::addMs(next.sinceValidMs, obs.elapsedMs);
        next.stale = next.sinceValidMs >= cfg.stalenessMs;
        // THE DWELL IS HELD, NOT ADVANCED AND NOT RESET. This is the whole
        // "the observation only exists while streaming" consequence: when the
        // evidence stops, the loop keeps the gain it is holding and waits. It
        // does not decay back toward the operator's baseline on the strength of
        // having heard nothing, because hearing nothing is not hearing clean.
        out.next = next;
        out.reason = next.stale ? AutoGainReason::Stale : AutoGainReason::Void;
        return out;
    }
    next.sinceValidMs = 0;
    next.stale = false;

    if (next.warmupRemaining > 0) {
        --next.warmupRemaining;
        out.next = next;
        out.reason = AutoGainReason::Warmup;
        return out;
    }

    // ---- attack ----
    if (w == AutoGainWindow::Hot || w == AutoGainWindow::Marginal) {
        next.cleanMs = 0;

        // Record the trip: the DEEPEST attenuation at which this band has been
        // seen to rail. A repeat within the backoff window widens both the
        // dwell and the margin, so a band that keeps tripping is probed less
        // often and from further away each time.
        if (next.tripOffsetDb < 0 || next.offsetDb >= next.tripOffsetDb) {
            const bool repeat = next.tripOffsetDb >= 0
                             && next.releasedSinceTrip
                             && next.sinceTripMs <= cfg.tripBackoffWindowMs;
            next.tripOffsetDb = next.offsetDb;
            next.sinceTripMs = 0;
            next.releasedSinceTrip = false;
            if (repeat) {
                next.tripMarginDb = next.tripMarginDb + cfg.tripMarginGrowthDb;
                if (next.tripMarginDb > cfg.tripMarginMaxDb) {
                    next.tripMarginDb = cfg.tripMarginMaxDb;
                }
                const std::int64_t base = next.dwellRequiredMs > 0 ? next.dwellRequiredMs
                                                                   : cfg.releaseDwellMs;
                // A BACKOFF MUST NEVER SHORTEN THE DWELL. Taking the cap
                // literally would do exactly that whenever the configured dwell
                // already exceeds it — which is the binary configuration's
                // normal case, where the hold IS the hysteresis.
                const std::int64_t cap = cfg.dwellBackoffMaxMs > base ? cfg.dwellBackoffMaxMs
                                                                      : base;
                next.dwellRequiredMs = base * 2 > cap ? cap : base * 2;
            } else {
                next.tripMarginDb = cfg.tripMarginDb;
            }
        }

        if (next.offsetDb >= ceiling) {
            next.atFloorHotMs = detail::addMs(next.atFloorHotMs, obs.elapsedMs);
            if (!next.floorAlarmed && next.atFloorHotMs >= cfg.floorAlarmMs) {
                next.floorAlarmed = true;
                out.warnFloorOnce = true;
            }
            out.next = next;
            out.reason = AutoGainReason::AtFloor;
            out.holdWarning = next.offsetDb > 0;
            return out;
        }
        next.atFloorHotMs = 0;

        if (next.sinceAttackMs < cfg.attackCooldownMs) {
            out.next = next;
            out.reason = AutoGainReason::Cooldown;
            return out;
        }

        // The first step after a quiet period is sized by the rate. Every
        // subsequent step is the ramp's, and BOTH share the cooldown above —
        // which is what stops chatter across a window boundary masquerading as
        // a series of first observations.
        const bool firstAfterQuiet = next.sinceAttackMs >= cfg.quietPeriodMs;
        int step = firstAfterQuiet
                     ? (w == AutoGainWindow::Hot ? cfg.firstStepHotDb
                                                 : cfg.firstStepMarginalDb)
                     : cfg.attackStepDb;
        if (step < 0) {
            step = 0;
        }
        const int room = ceiling - next.offsetDb;
        const int delta = step > room ? room : step;
        if (delta <= 0) {
            out.next = next;
            out.reason = AutoGainReason::Cooldown;
            return out;
        }
        next.offsetDb += delta;
        next.sinceAttackMs = 0;   // ADVANCED ONLY WHEN THE OFFSET ACTUALLY MOVED
        out.deltaDb += delta;
        out.next = next;
        out.reason = w == AutoGainWindow::Hot ? AutoGainReason::AttackHot
                                              : AutoGainReason::AttackMarginal;
        out.holdWarning = true;
        return out;
    }

    // ---- clean ----
    next.cleanMs = detail::addMs(next.cleanMs, obs.elapsedMs);
    next.atFloorHotMs = 0;
    next.floorAlarmed = false;

    // Propagation changes. Forget a trip nothing has confirmed for long enough,
    // or the loop never recovers a band that has gone quiet.
    if (next.tripOffsetDb >= 0 && next.sinceTripMs >= cfg.tripForgetMs) {
        next.tripOffsetDb = -1;
        next.tripMarginDb = 0;
        next.dwellRequiredMs = 0;
        next.releasedSinceTrip = false;
    }

    if (next.offsetDb <= 0) {
        out.next = next;
        out.reason = AutoGainReason::Idle;
        out.holdWarning = false;
        return out;
    }

    const int releaseFloor = next.tripOffsetDb >= 0
                               ? next.tripOffsetDb + next.tripMarginDb : 0;
    if (next.offsetDb <= releaseFloor) {
        out.next = next;
        out.reason = AutoGainReason::ReleaseHold;
        return out;
    }

    const std::int64_t dwell = next.dwellRequiredMs > 0 ? next.dwellRequiredMs
                                                        : cfg.releaseDwellMs;
    if (next.cleanMs < dwell) {
        out.next = next;
        out.reason = AutoGainReason::Dwell;
        return out;
    }
    if (next.sinceReleaseMs < cfg.releaseIntervalMs) {
        out.next = next;
        out.reason = AutoGainReason::Dwell;
        return out;
    }

    int step = cfg.releaseStepDb < 0 ? 0 : cfg.releaseStepDb;
    const int room = next.offsetDb - releaseFloor;
    const int delta = step > room ? room : step;
    if (delta <= 0) {
        out.next = next;
        out.reason = AutoGainReason::ReleaseHold;
        return out;
    }
    next.offsetDb -= delta;
    next.sinceReleaseMs = 0;
    // The loop has now given gain back. If the band clips again before it
    // forgets, THAT is a repeat and widens the backoff.
    next.releasedSinceTrip = true;
    out.deltaDb -= delta;
    out.next = next;
    out.reason = AutoGainReason::Release;
    out.holdWarning = next.offsetDb > 0;
    return out;
}

}  // namespace AetherSDR::hl2
