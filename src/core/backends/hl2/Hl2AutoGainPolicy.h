#pragma once

// The automatic receive-gain control law, as a pure function:
//
//     (state, observation, config) -> action
//
// No Qt, socket, clock or radio. Elapsed time is an input and the result is an
// instruction about state; Hl2Backend owns the timers and the register write.
// It is a header the backend evaluates (not a copy of it) so tests exercise the
// real decision.
//
// PARAMETERISED, NOT A FIXED SERVO. On ON8ST's station (#5354) 6 dB of LNA spans
// the whole 0 % -> 86 % clipping transition at 14.2 MHz, so a 3 dB step is half
// the plant's linear region and a per-band binary high/low switch may be the
// better feature. Step, thresholds, dwell, cooldown and trip memory are all
// configuration; binaryHighLowConfig() is the binary controller as one config.
//
// WHAT THE OBSERVATION IS. Response address 0 DATA[24] is `(&clip_cnt)`, the AND
// of a two-bit saturating counter cleared on every resp_rqst: it means AT LEAST
// THREE CLIP EVENTS IN ONE REPORTING INTERVAL, not "a sample railed" (#5354).
// So:
//
//   * `Clean` means "fewer than three clip events per interval", which at
//     76.8 MSPS can still be a lot of clipping. The wideband bandscope
//     (Hl2BandscopeHeadroom.h) supplies the magnitude and an early-warning knee
//     below the clip point; the bit supplies coverage of every sample. Veto and
//     magnitude; neither substitutes for the other.
//   * The observation exists only while streaming. At idle nothing clears the
//     counter and the bit is a latch of unknown age. When evidence stops, the
//     loop HOLDS its offset: no decay, no release, no dwell advance. A window
//     with too few observations is `Void`, and `Void` is not `Clean`.
//   * The rate (overloadSamples / samples, ~190 samples/s) is ordinal and a
//     lower bound, because the crossing can miss assertions. Hence three coarse
//     states, not a proportional law.
//   * Command responses displace the slot carrying this bit (up to half of
//     them), so a count is usable only with its denominator: `minSamples`.
//
// THE AXIS. `offsetDb` is a non-negative attenuation below the operator's
// baseline (Hl2GainSplit.h). The law cannot make the radio louder than the
// operator set; the only automatic move in the loud direction undoes its own.

#include <cstdint>

#include "core/backends/hl2/Hl2BandscopeHeadroom.h"

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
    // Clean, dwelt, and the bandscope HAS reported -- and what it reported is
    // not enough room for the step. The loop stays where it is and the reason
    // says which sensor stopped it, because "not enough headroom" and "not
    // clean long enough" call for different things from an operator.
    HeadroomHold,
    // The loop was configured to require a measured headroom before releasing
    // and there is no current reading. NOT the same as no headroom: it is no
    // answer, and the loop holds rather than guessing in either direction.
    HeadroomAbsent,
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

    // ---- PROBING RELEASE ---------------------------------------------------
    //
    // THE RELEASE CONDITION IS THE HARD PART, AND NOTHING ON THIS RADIO
    // MEASURES HEADROOM. The clip flag is an honest "you are too high" sensor
    // and nothing else: it says the converter railed, never how much room is
    // left below the rails. `RXA_ADC_PK` cannot stand in for it, because it
    // measures the post-DDC slice while the flag measures the pre-DDC full
    // spectrum -- a quiet 48 kHz slice reads as headroom while a broadcast
    // station saturates the converter (docs/HERMES.md 12.5).
    //
    // So the only way to find out whether the gain can come back is TO TRY IT
    // AND SEE. A release is therefore not a decision, it is a PROBE, and it can
    // fail. `probeConfirmMs` is how long the answer has to stay clean before
    // the probe is believed:
    //
    //   - a clip INSIDE that period is a FAILED probe. The attack branch puts
    //     the step straight back and, because the loop had released since its
    //     last trip, treats the clip as a REPEAT: the dwell -- which is what
    //     paces the next probe -- DOUBLES, up to `dwellBackoffMaxMs`.
    //   - a probe that survives it is believed: the interval goes back to base
    //     and `releasedSinceTrip` is cleared, so the NEXT clip is a fresh trip
    //     paced from base rather than a continuation of a backoff that belonged
    //     to a different hour of the day.
    //
    // THAT IS THE ASYMMETRY, AND IT IS THE POINT. While conditions are genuinely
    // hot the probes fail, the interval doubles, and the loop goes quiet. The
    // moment there is real headroom the first probe survives, the interval
    // collapses to base -- and because `cleanMs` is by then far past the dwell,
    // successive steps come one per `releaseIntervalMs`. Slow to give up on a
    // hot band; fast to reclaim gain once the band has actually gone quiet.
    //
    // ZERO DISABLES IT, which is the default: with `probeConfirmMs == 0` a
    // release is never confirmed, nothing resets the interval, and this law is
    // exactly the one that shipped before probing existed.
    std::int64_t probeConfirmMs = 0;

    // Whether the per-band trip memory binds the RELEASE FLOOR as well as the
    // backoff. True is the original behaviour: the loop will not return below
    // `tripOffsetDb + tripMarginDb`, which converges a hunt on a plant whose
    // knee sits still.
    //
    // PROBING SETS IT FALSE, AND THE REASON HAS BEEN NARROWED SINCE IT WAS
    // WRITTEN. This comment used to say the knee moves 10-20 dB between a quiet
    // afternoon and a loud evening. ON8ST WITHDREW THE DIURNAL ATTRIBUTION on
    // #5535 (2026-09-12): the return-to-baseline control fired, two of three
    // bands did not return, and the +/-2 dB floor quoted was two sweeps 90 min
    // apart on one night. Overnight repeatability was never measured.
    //
    // WHAT SURVIVES IS THE PART THIS FIELD ACTUALLY NEEDS, and he says so in the
    // same breath: "the range and the variability stand, the hour as cause does
    // not". The knee is not a constant. A remembered offset that permanently
    // floors the release is a lookup table keyed on a number that moves for
    // reasons nobody has established: dig 18 dB out once and the loop can never
    // return below 12 dB again. With the floor off, what stops the loop hunting
    // is the widening probe interval and the bounded cost of a failed probe --
    // a memory in TIME rather than in decibels, which is the only kind that
    // survives a knee whose movement is measured but unexplained.
    bool tripFloorBindsRelease = true;

    // ---- THE MEASURED-HEADROOM RELEASE (Hl2BandscopeHeadroom.h) ------------
    //
    // THIS IS WHAT THE BANDSCOPE CHANGES, AND IT CHANGES THE ONE THING THIS
    // HEADER PREVIOUSLY HAD TO GUESS AT.
    //
    // Read the probing-release comment below and note what it rests on:
    // "NOTHING ON THIS RADIO MEASURES HEADROOM ... so the only way to find out
    // whether the gain can come back is TO TRY IT AND SEE." That was true of
    // the clip flag and it is true of RXA_ADC_PK, and it is why a release had
    // to be a deliberate probe that costs a clipped window when it fails.
    //
    // The wideband bandscope measures it. It samples the SAME `rx_data`
    // register the clip flag is derived from, pre-DDC, across the whole
    // 0-38.4 MHz the converter sees -- so it answers "how far below the rail
    // is the loudest thing anywhere in the converter's span", including the
    // out-of-slice signal that is the usual cause and the one neither the
    // S-meter nor the post-DDC slice peak can see.
    //
    // With `requireHeadroomToRelease` a release stops being a gamble: the loop
    // gives gain back only when it has SEEN room for the whole step. The probe
    // machinery below is kept, unchanged, because a probe can still fail --
    // the gate's duty cycle can miss a transient between blocks -- so a failed
    // release still backs the interval off exactly as before.
    //
    // ABSENT IS NOT ZERO AND IT IS NOT INFINITY. When this is required and no
    // current reading exists, the loop HOLDS and reports HeadroomAbsent. That
    // is the same rule the Void branch already applies to the clip bit:
    // hearing nothing is not hearing clean, and it is certainly not hearing
    // room. It deliberately does NOT silently fall back to blind probing --
    // an operator who armed a measured mode did not ask for a deliberate clip.
    bool requireHeadroomToRelease = false;

    // The caller's own margin, ON TOP of the step and the sampling bias below.
    // Zero is a legitimate choice; the bias term is the one that must not be
    // zero, and it is separate for exactly that reason.
    double releaseHeadroomMarginDb = 0.0;

    // THE GATED PEAK UNDERSTATES THE TRUE PEAK, AND THIS IS THE BUDGET FOR IT.
    //
    // Hl2BandscopeHeadroom.h's gatedPeakBiasDb computes the bound: observing
    // 2048 samples a second instead of 76 800 000 costs about 3.77 dB of
    // expected maximum under a Gaussian model. The error is in the dangerous
    // direction -- it makes the band look quieter than it is -- so it is
    // budgeted as margin rather than corrected for. The caller sets this from
    // the gate period it actually runs, and MUST NOT leave it at zero while
    // requiring headroom: that would license steps the reading cannot support.
    double headroomBiasDb = 0.0;

    // A bandscope block carrying samples at a converter rail is a clip
    // observation in its own right -- the same register, the same thresholds,
    // just sampled -- so it may escalate an otherwise Clean window to Marginal
    // and let the attack branch act on it.
    //
    // IT ONLY EVER ESCALATES, AND ONLY FROM Clean. It cannot make a Hot window
    // milder, and it deliberately does not rescue a Void one: a Void window
    // means the loop is not being fed, and the existing rule for that -- hold
    // what you have -- is the safe one and is not this sensor's to overturn.
    bool headroomRailAttacks = false;

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

// ---------------------------------------------------------------------------
// PROBING RELEASE, as one particular AutoGainConfig
// ---------------------------------------------------------------------------
//
// ONE DETECTION WINDOW = MetisClient::kTelemetryMinIntervalMs = 100 ms. This is
// read out of the code, not chosen: Hl2Backend::publishTelemetry evaluates this
// function on each coalesced telemetryUpdated. Address 0 arrives ~190/s at
// 48 kHz with one receiver (~19 per window, ~9 worst case with command ACKs
// displacing slots), comfortably above minSamples. A failed probe therefore
// costs about one window (~100 ms) of a clipping converter.
//
//   probeStepDb = 6           The measured clean->clipping knee is 3-5 dB wide
//                             (ON8ST, d92-clip-observability); 6 dB clears it
//                             without stalling inside, and equals one attack
//                             step, so a probe undoes exactly one attack.
//   maxOffsetDb = 24          Four whole steps, so no move is a truncated
//                             remainder. Operator-owned. Likely deeper than the
//                             hardware: the LNA folds `& 0x1F` above code 31 and
//                             one board measured ~17.8 dB usable (#5535, open).
//                             Hl2GainSplit.h clamps to the register floor and
//                             reports the applied offset, and AtFloor lights the
//                             "attenuate ahead of the radio" warning.
//   baseProbeIntervalMs       30 s: one failed probe per interval is a 0.33 %
//     = 30000                 clip duty cycle before backoff, and still two
//                             orders of magnitude faster than the 10-20 dB knee
//                             drift across dawn/dusk. The struct default of 3 s
//                             would clip deliberately every 3 s all night.
//   maxProbeIntervalMs        Four doublings (30 -> 480 s): worst-case latency to
//     = 480000                notice the band went quiet, well under a dawn
//                             transition; ~0.02 % clipping overnight at the cap.
//   probeConfirmMs = 3000     d92's fixed-gain control swung 0 % -> 90 % between
//                             3 s blocks, so a shorter confirmation can sit
//                             inside a lull. 30 detection windows.
//   releaseIntervalMs = 3000  Same as confirm, so the next probe follows at once:
//                             the full 24 dB returns in ~12 s if the band allows.
//   attackCooldownMs, minSamples, unkeyHoldoffMs, warmupWindows, stalenessMs
//                             Defaults. The 200 ms cooldown is two windows, so a
//                             clip must persist into a fresh window before a
//                             second step; 24 dB still digs out in ~0.8 s.
//   tripBackoffWindowMs       A probe at the cap arrives maxProbeIntervalMs after
//     = 2 * maxProbeIntervalMs  its trip and must still count as a repeat.
//   tripForgetMs = 3600000    Longer than any reachable interval, so only a
//                             confirmed probe, band change or operator resets
//                             the interval to base.
[[nodiscard]] constexpr AutoGainConfig probingReleaseConfig(
    std::int64_t baseProbeIntervalMs = 30000,
    std::int64_t maxProbeIntervalMs = 480000,
    std::int64_t probeConfirmMs = 3000) noexcept
{
    AutoGainConfig c;
    // One quantum, both directions. A probe undoes exactly one attack step.
    c.firstStepHotDb = 6;
    c.firstStepMarginalDb = 6;
    c.attackStepDb = 6;
    c.releaseStepDb = 6;
    c.maxOffsetDb = 24;

    // The dwell IS the probe interval: `cleanMs` has to reach it before the
    // loop will try more gain, and the repeat-trip backoff already doubles it.
    c.releaseDwellMs = baseProbeIntervalMs;
    c.dwellBackoffMaxMs = maxProbeIntervalMs;
    c.probeConfirmMs = probeConfirmMs;
    c.releaseIntervalMs = probeConfirmMs;

    // A moving knee cannot be remembered in decibels; see tripFloorBindsRelease.
    c.tripFloorBindsRelease = false;
    c.tripMarginDb = 0;
    c.tripMarginGrowthDb = 0;
    c.tripMarginMaxDb = 0;

    c.tripBackoffWindowMs = maxProbeIntervalMs * 2;
    c.tripForgetMs = 3600000;
    return c;
}

// ---------------------------------------------------------------------------
// THE BANDSCOPE-FED RELEASE, as one particular AutoGainConfig
// ---------------------------------------------------------------------------
//
// probingReleaseConfig() with the release licensed by a MEASUREMENT instead of
// taken on spec. Everything that was argued for probing is kept and is not
// re-opened here; what changes is that the loop now has to have SEEN the room
// before it takes a step into it.
//
//   requireHeadroomToRelease = true
//       The whole point. See AutoGainConfig's comment.
//
//   headroomBiasDb -- NOT DEFAULTED, and it is the caller's to supply from the
//       gate period it actually runs (gatedPeakBiasDbForPeriod). It is a
//       parameter rather than a constant because a future change to
//       MetisClient::kBandscopeSampleMs must move it, and a literal 3.77 here
//       would silently stop matching the gate it describes. At the shipped
//       1000 ms period it is 3.77 dB.
//
//   releaseHeadroomMarginDb = 2
//       On top of the step and the bias. The bias bound is an expectation
//       under a Gaussian model and the model is a first approximation to a
//       real band, so a reading that exactly meets the requirement is not a
//       comfortable place to step from. 2 dB is a third of one probe step and
//       is the smallest margin that is not a rounding artefact. It is a
//       CHOICE and it is answerable to no measurement -- which is stated here
//       rather than dressed up, and is a thing #5535 may well want to set.
//
//   headroomRailAttacks = true
//       A block at the rail is a clip and the loop should act on it. Costs
//       nothing when the bandscope is off, because Absent never rails.
//
// WHAT IS DELIBERATELY LEFT ALONE, AND WHY THAT IS A QUESTION FOR #5535 AND
// NOT FOR THIS FILE:
//
// probingReleaseConfig() justifies its 30 s base interval by the COST OF A
// FAILED PROBE -- "one failed probe per interval is 100 ms of clipping per
// interval, so 30 s is a duty cycle of 0.33 %". A measurement-licensed release
// is not expected to fail, so that justification genuinely weakens, and a
// shorter interval would reclaim gain faster after a band goes quiet.
//
// It is NOT shortened here. How fast an automatic control is allowed to give
// gain back is a control decision with an operator-visible consequence, the
// evidence that would size it is a live-antenna measurement nobody has taken
// (the study's Procedure C), and #5535 is unanswered. Changing it would be
// deciding in code a thing the RFC exists to decide. The interval stays at
// probing's, the loop is strictly more cautious than probing was, and the
// number is one line to change once there is a ruling to change it to.
[[nodiscard]] inline AutoGainConfig bandscopeReleaseConfig(
    double headroomBiasDb,
    double releaseHeadroomMarginDb = 2.0) noexcept
{
    AutoGainConfig c = probingReleaseConfig();
    c.requireHeadroomToRelease = true;
    c.headroomBiasDb = headroomBiasDb;
    c.releaseHeadroomMarginDb = releaseHeadroomMarginDb;
    c.headroomRailAttacks = true;
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

    // THE WIDEBAND HEADROOM READING, or Absent when there is none.
    //
    // Absent is the normal case and must stay cheap: the bandscope is off by
    // default, and a loop configured without requireHeadroomToRelease ignores
    // this field entirely and behaves exactly as it did before the sensor
    // existed. That equivalence is asserted in hl2_auto_gain_policy_test.
    //
    // The caller builds this with bandscopeHeadroom() from the newest accepted
    // block and its age; this function has no clock and does not classify.
    HeadroomObservation headroom;
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
    AutoGainWindow w = classifyWindow(obs.samples, obs.overloadSamples, cfg);

    // A BANDSCOPE BLOCK THAT RAILED IS A CLIP, and the clip bit may simply not
    // have been in the window that carried it -- the flag is latched and
    // cleared by the EP6 response cycle, on its own cadence. This is the same
    // register and the same thresholds, so acting on it is not a second
    // opinion, it is the same opinion arriving by a different route.
    //
    // ESCALATION ONLY, AND ONLY FROM Clean. It never softens a Hot window and
    // never rescues a Void one; see AutoGainConfig::headroomRailAttacks. The
    // direction is the safe one -- the loop can only become MORE cautious on
    // this evidence, never less.
    if (cfg.headroomRailAttacks && obs.headroom.railed()
        && w == AutoGainWindow::Clean) {
        w = AutoGainWindow::Marginal;
    }

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

    // ---- a probe in flight is believed, or it is not ----
    //
    // A release is the loop ASKING whether the headroom it has no way to
    // measure has come back. `probeConfirmMs` is how long the answer has to
    // stay clean before the question counts as answered:
    //
    //   - a clip before then never reaches here. It takes the attack branch
    //     above, which sees `releasedSinceTrip` still set, calls the trip a
    //     REPEAT, and doubles the interval that paces the next probe.
    //   - reaching here with the period elapsed is the probe SURVIVING. The
    //     interval goes back to base and the flag clears, so the next clip is a
    //     fresh trip rather than the continuation of a backoff that was earned
    //     under conditions that have since changed.
    //
    // BOTH CLOCKS ARE REQUIRED, and they are not the same clock.
    // `sinceReleaseMs` says the probe has been in flight long enough;
    // `cleanMs` says that whole time was spent OBSERVING a clean converter, and
    // it is reset by keying, by the post-unkey hold-off and by warmup. Without
    // the second, a transmission in the middle of a probe would let wall time
    // confirm a probe that was never watched.
    //
    // THIS RUNS BEFORE THE ZERO-OFFSET RETURN BELOW ON PURPOSE. A probe that
    // takes the offset all the way back to the operator's baseline is the one
    // most worth confirming, and returning Idle first would leave the loop
    // carrying a stale backoff forever.
    if (cfg.probeConfirmMs > 0 && next.releasedSinceTrip
        && next.sinceReleaseMs >= cfg.probeConfirmMs
        && next.cleanMs >= cfg.probeConfirmMs) {
        next.releasedSinceTrip = false;
        next.dwellRequiredMs = 0;
    }

    if (next.offsetDb <= 0) {
        out.next = next;
        out.reason = AutoGainReason::Idle;
        out.holdWarning = false;
        return out;
    }

    // WHERE THE REMEMBERED TRIP IS ALLOWED TO STOP A RELEASE, and where it is
    // not. With `tripFloorBindsRelease` the memory is a decibel and the loop
    // will not return below it; without it the memory is the widening probe
    // interval instead, and the only floor is the operator's own baseline. See
    // the field's comment: a knee that moves cannot be remembered in decibels.
    const int releaseFloor = (cfg.tripFloorBindsRelease && next.tripOffsetDb >= 0)
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

    // ---- THE MEASURED-HEADROOM LICENCE ------------------------------------
    //
    // Everything above this point is the clip flag's business: the converter
    // has not railed for long enough, and the pacing allows another step. That
    // establishes only that the loop is ALLOWED to try. It does not establish
    // that there is room, and before the bandscope nothing could.
    //
    // Here the loop asks the wideband reading whether `delta` dB of gain
    // actually fits -- against the step, the gate's sampling bias and the
    // configured margin (Hl2BandscopeHeadroom.h::headroomLicensesStepDb).
    //
    // THE LICENCE IS CHECKED AGAINST `delta`, NOT `cfg.releaseStepDb`, because
    // delta is what will actually be given back once the release floor has
    // truncated the step. Asking about a step the loop is not taking would
    // refuse releases that fit.
    //
    // Both refusals below hold the offset and advance nothing: no release
    // happened, so `sinceReleaseMs` keeps running and the next tick asks
    // again. A refusal is not a failed probe and must not earn a backoff --
    // the loop never moved, so there is nothing for the band to have punished.
    if (cfg.requireHeadroomToRelease) {
        if (!obs.headroom.isMeasurement()) {
            out.next = next;
            out.reason = AutoGainReason::HeadroomAbsent;
            return out;
        }
        if (!headroomLicensesStepDb(obs.headroom,
                                    static_cast<double>(delta),
                                    cfg.releaseHeadroomMarginDb,
                                    cfg.headroomBiasDb)) {
            out.next = next;
            out.reason = AutoGainReason::HeadroomHold;
            return out;
        }
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
