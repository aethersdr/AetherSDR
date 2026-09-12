#pragma once

// CONVERTER HEADROOM FROM THE WIDEBAND BANDSCOPE, as a pure decision.
//
// The wideband bandscope (endpoint 0x04, MetisProtocol.h's Ep4Stats) is the
// first observation on this radio that answers "how much room is left below
// the rails" rather than only "you are already too high". This header turns
// one accepted bandscope block into that answer, and states precisely what
// the answer is worth.
//
// ---------------------------------------------------------------------------
// WHY THIS IS THE RIGHT SENSOR, AND IT IS ABOUT WHAT IS OUTSIDE THE SLICE
// ---------------------------------------------------------------------------
//
// The bandscope samples `rx_data` -- the AD9866's output register, PRE-DDC,
// pre-decimation, pre-NCO. It therefore sees the whole first Nyquist zone,
// DC..38.4 MHz, exactly as the converter does.
//
// That is the property that matters, and neither of the two level readings
// this application already had can supply it:
//
//   * THE S-METER is computed after the DDC, after decimation and after the
//     demodulator. It describes one slice.
//   * WDSP's RXA_ADC_PK is also POST-DDC (RXA.c's adcmeter runs on the IQ
//     entering the chain). It describes one slice too, in dB relative to WIRE
//     full scale, with the DDC's own unquantified processing gain between it
//     and the converter.
//
// So a broadcast station 20 MHz away can saturate the converter while the
// operator's slice sits 40 dB below full scale, and NOTHING the operator can
// see will say so. docs/HERMES.md 12.5 calls the pre-DDC/post-DDC pairing "the
// single most useful diagnostic pairing on the HL2" for this reason. The
// bandscope is the pre-DDC half, with a MAGNITUDE rather than a boolean.
//
// ---------------------------------------------------------------------------
// WHAT IS CALIBRATED HERE, AND IT IS EXACTLY ONE THING
// ---------------------------------------------------------------------------
//
// NOTHING IN THIS FILE IS ANTENNA-REFERRED. No dBm, no microvolts, no LNA
// correction, no `Hl2DbReference` (which is per-slice and defaults
// `isCalibrated()` false). Nobody has compared a bandscope reading against a
// signal generator, and the study's Procedure C -- which would -- needs a live
// antenna and has not been run.
//
// What IS exact, and it is exact BY CONSTRUCTION rather than by measurement,
// is the relationship between this reading and the converter's own clip
// threshold. `ad9866.v` derives all three from the same register:
//
//     always @ (posedge clk) rx_data <= rx_data_assemble;
//     assign rxclipp    = (rx_data == 12'b011111111111);   // +2047
//     assign rxclipn    = (rx_data == 12'b100000000000);   // -2048
//     assign rxgoodlvlp = (rx_data[11:9] == 3'b011);       // >= +1536
//     assign rxgoodlvln = (rx_data[11:9] == 3'b100);       // <= -1536
//
// ONE REGISTER, THREE CONSUMERS. The bandscope is not *like* what the clip
// detector sees; it is that, sampled. So "this block peaked 6 dB below the
// code at which rxclip fires" is a true statement with no reference in it,
// and it is the only kind of statement this header makes.
//
// The unit is therefore written "dB below the converter's clip point" and
// never "dBFS" alone, because the second invites being read as a calibrated
// absolute. Every label that leaves here must survive an operator asking
// "below what?".
//
// ---------------------------------------------------------------------------
// THE GATE'S DUTY CYCLE BIASES THE PEAK, AND THE BIAS IS COMPUTABLE
// ---------------------------------------------------------------------------
//
// `rxclip` inspects EVERY sample. The bandscope, gated to one block a second
// by MetisClient::kBandscopeSampleMs, inspects 2048 consecutive samples out of
// every 76 800 000. That is 0.0027 % coverage, and it has two consequences
// that pull in opposite directions and must both be stated:
//
//   1. IT CAN MISS A TRANSIENT ENTIRELY. A lightning crash or a key-click that
//      rails the converter between blocks leaves no trace in a reading taken
//      400 ms later. The clip flag still sees it -- enabling the bandscope
//      removes nothing -- which is why a consumer must treat the flag as the
//      VETO and this reading as the MAGNITUDE, never the other way round.
//
//   2. IT SYSTEMATICALLY UNDERESTIMATES THE PEAK, because the maximum of a
//      sample is a function of how many samples you took. That is not a risk;
//      it is a bias, and `gatedPeakBiasDb` below computes its bound.
//
// A consumer that acts in the loud direction on this number MUST budget (2) as
// margin. That is what `headroomLicensesStepDb` is for, and it is the whole
// reason this header exists rather than a bare `-peakDbfs()` at the call site.
//
// ---------------------------------------------------------------------------
// A SEAM RATHER THAN AN INLINE CONDITION
// ---------------------------------------------------------------------------
//
// For Hl2OverloadPolicy.h's reason, which binds harder here: every branch below
// is otherwise reachable only by driving a real converter into a real overload
// with a real out-of-slice signal, which is not a thing a test suite can
// arrange. As pure functions they are reachable by arithmetic.
//
// No Qt, no clock, no radio. Age is an INPUT.

#include <cmath>
#include <cstdint>

#include "core/backends/hl2/MetisProtocol.h"   // Ep4Stats, kEp4FullScale

namespace AetherSDR::hl2 {

// ---- freshness -------------------------------------------------------------

// How old a bandscope block may be and still describe "now".
//
// A DISPLAY AND CONTROL BOUNDARY, NOT A CALIBRATED ONE -- the same kind of
// number as Hl2AdcPairing.h's slice-hot threshold, and chosen the same way.
// The gate produces one block per MetisClient::kBandscopeSampleMs (1000 ms),
// so anything inside two gate periods is the current or the previous block and
// anything beyond it means the gate stopped delivering. Three periods is that
// with one period of slack for a late I/O thread, which is the same tolerance
// bandscopeGuardMs() already budgets for the arming path.
//
// EXPIRY IS NOT ZERO HEADROOM. An expired observation is ABSENT -- see
// BandscopeHeadroom::Absent -- and a consumer must treat it as "not reported",
// never as "no room". The two are opposite instructions.
inline constexpr std::int64_t kHeadroomMaxAgeMs = 3000;

// ---- the observation, classified ------------------------------------------

enum class BandscopeHeadroom {
    // No block has arrived, the block carried no samples, or the newest block
    // is older than kHeadroomMaxAgeMs. NOT a level, and must render as "not
    // reported" rather than as any number.
    Absent,
    // The block carried samples at a converter rail, counted with ad9866.v's
    // OWN asymmetric predicate. Headroom is zero and the reading cannot say
    // how far past zero -- a railed code is pinned, so the sensor has no
    // magnitude in this direction. The clip flag says the same thing with
    // 100 % sample coverage; this adds only the count within the block.
    AtRail,
    // The block's peak reached rxgoodlvl's knee (|code| >= 1536, -2.50 dB
    // below the clip point) without reaching a rail. The converter is close.
    // THIS IS THE EARLY WARNING THE BOOLEAN CANNOT GIVE: the clip flag is
    // still clear here and will stay clear until the rail is actually hit.
    NearRail,
    // A real headroom figure below the knee. The only state that carries a
    // usable magnitude.
    Measured,
};

struct HeadroomObservation {
    BandscopeHeadroom state = BandscopeHeadroom::Absent;
    // dB BELOW THE CONVERTER'S CLIP POINT, never negative. Zero means "at or
    // past the rail". Meaningless unless `state` is NearRail or Measured; see
    // the file comment for why this is not a dBFS absolute.
    double headroomDb = 0.0;
    // Samples at a rail within this block, out of kEp4BlockSamples.
    int clippedSamples = 0;
    // Age of the block this came from, as the caller measured it.
    std::int64_t ageMs = 0;
    [[nodiscard]] constexpr bool isMeasurement() const noexcept
    {
        return state == BandscopeHeadroom::NearRail
            || state == BandscopeHeadroom::Measured;
    }
    // Did the converter rail inside the observed window? Distinct from
    // isMeasurement(): this is evidence of overload, which is the one thing
    // this sensor may report in the same direction as the clip flag.
    [[nodiscard]] constexpr bool railed() const noexcept
    {
        return state == BandscopeHeadroom::AtRail;
    }
};

// The gateware's good-level code, verbatim from ad9866.v's rxgoodlvl: the
// predicate is `rx_data[11:9] == 3'b011` / `3'b100`, which is |code| >= 1536.
inline constexpr int kGoodLevelCode = 1536;

// The same knee expressed as headroom below the clip point, for display and
// for the test that pins our dB scale against the converter's:
// 20*log10(1536/2048) = 2.4988 dB. Written as the arithmetic rather than as a
// literal so it cannot drift from kEp4FullScale.
//
// CLASSIFICATION DOES NOT USE THIS. bandscopeHeadroom() compares the integer
// code against kGoodLevelCode instead, which is the gateware's own predicate
// exactly, needs no floating point, and cannot disagree with rxgoodlvl by a
// rounding step at the boundary.
inline const double kGoodLevelKneeHeadroomDb =
    -20.0 * std::log10(static_cast<double>(kGoodLevelCode)
                       / static_cast<double>(kEp4FullScale));

// One accepted bandscope block plus its age, classified.
//
// `ageMs` is an input because this header owns no clock. A negative age means
// "never observed" and is the same answer as too old.
[[nodiscard]] inline HeadroomObservation bandscopeHeadroom(
    const Ep4Stats& block,
    std::int64_t ageMs,
    std::int64_t maxAgeMs = kHeadroomMaxAgeMs) noexcept
{
    HeadroomObservation out;
    out.ageMs = ageMs;
    if (block.samples <= 0 || ageMs < 0 || ageMs > maxAgeMs) {
        return out;                       // Absent
    }
    out.clippedSamples = block.clippedSamples;
    if (block.clippedSamples > 0) {
        out.state = BandscopeHeadroom::AtRail;
        out.headroomDb = 0.0;
        return out;
    }
    // peakDbfs() is <= 0 on the converter's own scale, so negating it gives
    // headroom below the clip point directly. Clamp at zero: a peak code of
    // kEp4FullScale would read 0.00 and must not come back as -0.0.
    const double headroom = -block.peakDbfs();
    out.headroomDb = headroom < 0.0 ? 0.0 : headroom;
    out.state = block.peakAbs >= kGoodLevelCode ? BandscopeHeadroom::NearRail
                                                : BandscopeHeadroom::Measured;
    return out;
}

// ---- the sampling bias, as a bound ----------------------------------------

// How much a peak taken over `observedSamplesPerSecond` samples UNDERSTATES
// the peak that `fullRateSamplesPerSecond` samples would have found, in dB,
// returned as a POSITIVE number of dB of understatement.
//
// THE MODEL IS STATED AND IT IS AN UPPER BOUND, NOT A CORRECTION.
//
// Under a Gaussian model -- the right first model for a wide-open HF front end
// seeing the aggregate of many signals plus noise -- the expected maximum of N
// samples is sigma*sqrt(2 ln N). The ratio of two such expectations is the
// bias, and because it is logarithmic in N it is bounded and slowly varying:
// it moves about 2 dB across a hundred-fold change in duty cycle.
//
// THE BIAS IS ZERO FOR A DETERMINISTIC SIGNAL. A strong broadcast carrier has
// a periodic envelope, and 2048 samples at 76.8 MSPS resolve every beat faster
// than 37.5 kHz. So on a real band the true error sits somewhere between zero
// and this figure, and WHERE is a measurement nobody has made.
//
// Therefore: budget it as MARGIN, never subtract it as a correction. A
// consumer that added this number back to a reading would be claiming a
// precision this observation does not have. `headroomLicensesStepDb` uses it
// the honest way.
//
// Returns 0 when either count is not usable, which is the conservative answer
// for a margin (a caller adding it gets no licence it had not already earned).
[[nodiscard]] inline double gatedPeakBiasDb(
    double observedSamplesPerSecond,
    double fullRateSamplesPerSecond = kAdcSampleRateHz) noexcept
{
    if (!(observedSamplesPerSecond > 1.0) || !(fullRateSamplesPerSecond > 1.0)
        || observedSamplesPerSecond >= fullRateSamplesPerSecond) {
        return 0.0;
    }
    const double expectedMaxObserved = std::sqrt(2.0 * std::log(observedSamplesPerSecond));
    const double expectedMaxFull     = std::sqrt(2.0 * std::log(fullRateSamplesPerSecond));
    // Positive: how far BELOW the full-rate peak the gated one is expected to
    // land.
    return -20.0 * std::log10(expectedMaxObserved / expectedMaxFull);
}

// The bias bound for the gate as MetisClient actually runs it: one block of
// kEp4BlockSamples per `gatePeriodMs`.
[[nodiscard]] inline double gatedPeakBiasDbForPeriod(std::int64_t gatePeriodMs) noexcept
{
    if (gatePeriodMs <= 0) {
        return 0.0;
    }
    const double perSecond = static_cast<double>(kEp4BlockSamples)
                           * 1000.0 / static_cast<double>(gatePeriodMs);
    return gatedPeakBiasDb(perSecond);
}

// ---- what the reading licenses --------------------------------------------

// May a consumer take `stepDb` dB back in the LOUD direction on this evidence?
//
// The rule, and every term in it is load-bearing:
//
//     measured headroom  >=  the step itself
//                         +  the gate's sampling-bias bound
//                         +  the caller's own margin
//
//   * THE STEP ITSELF, because taking back N dB of attenuation raises the
//     converter's input by N dB, and a step into less than N dB of room is a
//     step into the rail by construction.
//   * THE BIAS BOUND, because the gated peak is an UNDERESTIMATE and the
//     direction of that error is exactly the dangerous one: it makes the band
//     look quieter than it is. Budgeting the bound means the licence survives
//     the worst case the model admits.
//   * THE CALLER'S MARGIN, which is the caller's business and not this
//     header's -- a control loop and a display want different amounts of it.
//
// AN ABSENT OBSERVATION LICENSES NOTHING. Not "no room" -- no answer. The
// caller must then fall back to whatever it did before this sensor existed,
// and must not read the refusal as a measurement.
//
// A NEARRAIL OBSERVATION IS STILL A MEASUREMENT and goes through the same
// arithmetic; it will simply fail it for any useful step, which is correct.
[[nodiscard]] inline bool headroomLicensesStepDb(const HeadroomObservation& obs,
                                                 double stepDb,
                                                 double marginDb,
                                                 double biasDb) noexcept
{
    if (!obs.isMeasurement()) {
        return false;
    }
    if (stepDb <= 0.0) {
        return false;                     // not a step in the loud direction
    }
    const double required = stepDb
                          + (biasDb > 0.0 ? biasDb : 0.0)
                          + (marginDb > 0.0 ? marginDb : 0.0);
    return obs.headroomDb >= required;
}

// ---- the pairing with the clip flag ---------------------------------------

// How the gated bandscope block and the CONTINUOUS clip flag compare.
//
// Both are pre-DDC and both come off `rx_data`, so unlike the WDSP pairing in
// Hl2AdcPairing.h these two are commensurable by construction (see the file
// comment). What differs is COVERAGE -- 100 % against 0.0027 % -- and the
// disagreement that produces is diagnostic rather than a fault.
enum class BandscopeClipAgreement {
    // One side has not reported.
    Unknown,
    // Flag clear, block below the knee. The uninteresting and normal case.
    Clear,
    // Flag clear, but the sampled block reached the good-level knee. The
    // converter is closer than the boolean is able to say. EARLY WARNING.
    ApproachingRail,
    // Both say it railed. The block adds the magnitude the flag never had.
    Agreed,
    // THE CASE THAT MUST NOT BE MISREAD. The flag says the converter railed
    // and the sampled block is clean -- because the overload fell in the
    // 99.997 % of the time the gate was not looking. The block is NOT evidence
    // against the flag; the flag has total coverage and this does not. A
    // consumer that "resolved" this in the block's favour would be releasing
    // gain into a converter it had just been told was clipping.
    ClippedBetweenBlocks,
};

// `haveFlag` is false until EP6 response address 0 has been seen at all, which
// is a different state from "seen, and clear" -- Hl2Telemetry keeps those apart
// with std::optional and so does this.
[[nodiscard]] inline BandscopeClipAgreement bandscopeClipAgreement(
    const HeadroomObservation& obs, bool haveFlag, bool flagOverload) noexcept
{
    if (!haveFlag || obs.state == BandscopeHeadroom::Absent) {
        return BandscopeClipAgreement::Unknown;
    }
    if (flagOverload) {
        return obs.railed() ? BandscopeClipAgreement::Agreed
                            : BandscopeClipAgreement::ClippedBetweenBlocks;
    }
    if (obs.railed()) {
        // The block railed and the flag did not. Not a contradiction: the flag
        // is latched and cleared by the EP6 response cycle, so a rail inside
        // this block may have been reported in the window before the one the
        // caller is holding. Treat it as the converter being at the rail --
        // the safe direction, and the one the block has direct evidence for.
        return BandscopeClipAgreement::Agreed;
    }
    return obs.state == BandscopeHeadroom::NearRail
             ? BandscopeClipAgreement::ApproachingRail
             : BandscopeClipAgreement::Clear;
}

}  // namespace AetherSDR::hl2
