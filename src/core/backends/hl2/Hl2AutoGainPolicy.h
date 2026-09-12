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
// RETRACTED PREMISE, CORRECTED HERE RATHER THAN QUIETLY DROPPED. An earlier
// version of this comment said the overload bit meant "the converter railed",
// and the control law below was sized against that reading. It is not what the
// bit means, the correction was posted on aethersdr/AetherSDR#5354, and it
// matters to every threshold in this file.
//
// Response address 0's DATA[24] is `(&clip_cnt)` -- the reduction AND of a
// TWO-BIT SATURATING counter that is cleared on every `resp_rqst`. A reduction
// AND is true only when both bits are set, i.e. only when the counter has
// SATURATED at 3. So the bit does not say "a sample railed". It says:
//
//     AT LEAST THREE CLIP EVENTS OCCURRED IN ONE REPORTING INTERVAL.
//
// And at idle there is no `resp_rqst`, so nothing clears it: the bit is then an
// uncleared latch of unknown age rather than a level.
//
// TWO CONSEQUENCES, AND THE SECOND IS WHY THIS FILE NOW HAS A SECOND SENSOR:
//
//   * `Clean` DOES NOT MEAN "NOT CLIPPING". It means "fewer than three clip
//     events per reporting interval", which on a converter running at
//     76.8 MSPS is a great deal of clipping. The loop can therefore sit in
//     Clean while the front end is being hit, and no amount of tuning the
//     thresholds below can recover what the bit never carried.
//   * THE RATE IS COARSER THAN IT LOOKS. `overloadSamples / samples` is a rate
//     of threshold crossings of a three-event counter, not a rate of clipping.
//     It remains ORDINAL -- more is worse -- which is why the three-state
//     quantisation survives the correction and a proportional law still would
//     not. It is not a magnitude and never was.
//
// The wideband bandscope (Hl2BandscopeHeadroom.h) is the answer to both. It
// reads the same `rx_data` register the counter is derived from, so it is
// commensurable with it by construction, and it carries an actual magnitude
// with an early-warning knee BELOW the clip point. What survives from the bit
// is its coverage: it inspects every sample and the bandscope inspects 0.0027 %
// of them. Veto and magnitude, and neither substitutes for the other.
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
// Every constant below is a choice, and every choice is answerable to a
// measurement or to a stated piece of arithmetic. None of them is Zeus's --
// that client's plant is a different converter behind a different front end,
// and its numbers were never measured here.
//
// THE DETECTION WINDOW IS NOT A CHOICE AT ALL; IT IS READ OUT OF THE CODE, and
// the whole scheme rests on it. The overload bit rides the EP6 C&C bytes, which
// `MetisClient`'s receive loop parses on every datagram and accumulates into
// `Hl2Telemetry::adcSamples` / `adcOverloadSamples`; it publishes them on
// `telemetryUpdated`, coalesced by `MetisClient::kTelemetryMinIntervalMs`, and
// `Hl2Backend::publishTelemetry` evaluates THIS FUNCTION on that publish and no
// other clock. So:
//
//     ONE DETECTION WINDOW = kTelemetryMinIntervalMs = 100 ms.
//
// Response address 0 arrives once every two EP6 datagrams, which at 48 kHz with
// one receiver is ~190 a second -- ~19 per window, and ~9 in the worst case
// where the radio displaces every other classic slot with a command ACK. Both
// are comfortably above `minSamples`, so the window is a real denominator and
// not a thin one. Higher sample rates and more receivers only raise it.
//
// THE COST OF A FAILED PROBE IS THEREFORE ONE WINDOW: the step goes on at the
// end of window N, the clip is observed across window N+1, and the step comes
// back off at its end. Roughly 100 ms of a railed converter, plus the few
// milliseconds it takes the gain bank to come round in `MetisClient`'s C&C
// rotation. That is the number the rest of this scheme is sized against.
//
//   probeStepDb = 6
//       The clean->clipping transition measured 3-5 dB wide, median 4, on
//       ON8ST's station (`d92-clip-observability`). One 6 dB move clears the
//       knee outright and cannot stall inside it, where a 3 dB move can
//       oscillate on the same edge. It is also the step the attack already
//       uses, and a probe that does not undo exactly one attack step is not a
//       probe of anything.
//
//   maxOffsetDb = 24
//       Four whole probe steps. Chosen so that EVERY move the loop makes is a
//       full 6 dB and never a remainder truncated against the ceiling -- a 2 dB
//       remainder is narrower than the measured knee and could stall inside it.
//       The operator owns this number.
//
//       AND IT IS PROBABLY LARGER THAN THE HARDWARE HAS. This 24 was sized
//       when the LNA axis was believed to span 31 dB. ON8ST retracted that on
//       #5535 (2026-09-12): the gain folds `& 0x1F` above code 31
//       (Hermes-Lite2 #177, design intent per softerhardware) and codes 28-31
//       sit within 0.07 dB on his board, leaving about 17.8 dB USABLE -- one
//       board, measured once, confirmed by nobody else.
//
//       IT IS NOT CHANGED HERE, deliberately. How deep an automatic control
//       may dig is one of exactly two numbers the operator owns, the
//       replacement figure rests on a single unreplicated board, and #5535 is
//       unanswered. Picking a new ceiling from one measurement would be
//       deciding in code the thing the RFC exists to decide. What the loop
//       does meanwhile is safe rather than silent: Hl2GainSplit.h clamps to
//       the register floor and reports the offset ACTUALLY applied, and the
//       AtFloor branch below lights the "the front end needs attenuation ahead
//       of the radio" warning rather than attacking into a fold forever.
//
//   baseProbeIntervalMs = 30000
//       The floor on it is the cost: one failed probe per interval is 100 ms of
//       clipping per interval, so 30 s is a duty cycle of 0.33 % -- one clipped
//       window in three hundred -- BEFORE the backoff, which only lowers it.
//       The ceiling on it is the thing being tracked: the knee moves 10-20 dB
//       across a dawn or dusk transition lasting tens of minutes, so 30 s is
//       two orders of magnitude faster than the drift and cannot lag it. The
//       shipped default of 3000 is wrong here by exactly that argument: a
//       deliberate clip every three seconds all night is not a feature.
//
//   maxProbeIntervalMs = 480000
//       Four doublings from base (30 -> 60 -> 120 -> 240 -> 480 s). It is the
//       worst-case latency with which the loop can notice that the band has
//       gone quiet, so it has to be comfortably shorter than the transition it
//       must not sleep through; eight minutes against a dawn that takes tens of
//       minutes has the margin. It bounds the steady-state cost too: an eight
//       hour night spent entirely at the cap is ~60 probes, ~6 s of clipping in
//       28800 s, 0.02 %.
//
//   probeConfirmMs = 3000
//       How long a probe has to survive to be believed. It cannot be short:
//       `d92`'s same-gain negative control alternated 3 s blocks AT A FIXED
//       GAIN and saw the observed clip rate swing 0 % -> 90 % between them, so
//       a confirmation shorter than one of those blocks can sit entirely inside
//       a lull and call it headroom. 3 s is one such block -- the shortest
//       period the bench has any evidence about at all. It is 30 detection
//       windows.
//
//   releaseIntervalMs = 3000
//       Deliberately the same number, so that once a probe is confirmed the
//       next one follows immediately: the reclaim rate is one 6 dB step per
//       confirmation period, and the full 24 dB comes back in about twelve
//       seconds if the band allows it. This is the fast half of the asymmetry.
//
//   attackCooldownMs, minSamples, unkeyHoldoffMs, warmupWindows, stalenessMs
//       Left at the defaults, which were argued elsewhere in this header and
//       are not probing's to re-open. The 200 ms cooldown is two detection
//       windows, so the loop must see a clip persist into a fresh window before
//       taking a second step, and still digs the full 24 dB out in ~0.8 s.
//
//   tripBackoffWindowMs = 2 * maxProbeIntervalMs
//       A bookkeeping consequence, not a control choice: a failed probe taken
//       at the cap arrives `maxProbeIntervalMs` after the trip it is probing
//       from, and it has to still count as a repeat or the backoff would stop
//       compounding exactly where it matters most.
//
//   tripForgetMs = 3600000
//       Also bookkeeping. Forgetting a trip resets the interval to base, and
//       nothing may do that except a CONFIRMED PROBE, a band change, or the
//       operator. An hour is beyond any interval the backoff can reach, so in
//       this configuration probing supersedes forgetting rather than racing it.
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
