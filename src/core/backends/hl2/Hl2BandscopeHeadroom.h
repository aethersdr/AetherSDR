#pragma once

// Converter headroom from one accepted EP4 bandscope block (MetisProtocol.h
// Ep4Stats), as pure clock-free functions. The bandscope samples ad9866.v's
// pre-DDC `rx_data`, so it sees out-of-slice stations that the post-DDC
// S-meter and RXA_ADC_PK miss (docs/HERMES.md 12.5). The clip threshold comes
// from the same register, so units are "dB below the clip point", never dBFS:
//     assign rxclipp    = (rx_data == 12'b011111111111);   // +2047
//     assign rxclipn    = (rx_data == 12'b100000000000);   // -2048
//     assign rxgoodlvlp = (rx_data[11:9] == 3'b011);       // >= +1536
//     assign rxgoodlvln = (rx_data[11:9] == 3'b100);       // <= -1536
// One 2048-sample block per MetisClient::kBandscopeSampleMs covers 0.0027 % of
// samples: the clip flag (every sample) is the veto, this is the magnitude, and
// the peak is underestimated (gatedPeakBiasDb() bounds it).

#include <cmath>
#include <cstdint>

#include "core/backends/hl2/MetisProtocol.h"   // Ep4Stats, kEp4FullScale

namespace AetherSDR::hl2 {

// ---- freshness -------------------------------------------------------------

// How old a bandscope block may be and still describe "now": three gate
// periods (kBandscopeSampleMs = 1000 ms), i.e. current or previous block plus
// one period of slack, matching bandscopeGuardMs(). A display/control
// boundary, not calibrated. Expired means Absent ("not reported"), never zero
// headroom.
inline constexpr std::int64_t kHeadroomMaxAgeMs = 3000;

// The single "is this block current" predicate shared by every reader
// (bandscopeHeadroom(), Hl2Backend::healthSnapshot()) so they cannot disagree.
// `ageMs < 0` means never observed, the same Absent as too old.
[[nodiscard]] inline constexpr bool bandscopeBlockIsCurrent(
    const Ep4Stats& block,
    std::int64_t ageMs,
    std::int64_t maxAgeMs = kHeadroomMaxAgeMs) noexcept
{
    return block.samples > 0 && ageMs >= 0 && ageMs <= maxAgeMs;
}

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
    // The same predicate the health rows now use, called rather than restated,
    // so the two cannot drift apart again.
    if (!bandscopeBlockIsCurrent(block, ageMs, maxAgeMs)) {
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

// How many dB a peak over `observedSamplesPerSecond` samples understates the
// full-rate peak (positive). Gaussian model: expected max of N samples is
// sigma*sqrt(2 ln N), so the ratio is logarithmic in N (~2 dB per 100x duty).
// For a deterministic signal (e.g. a broadcast carrier) the bias is ~0, so the
// true error lies between 0 and this: an upper bound to budget as margin,
// never a correction to add back. Returns 0 for unusable counts.
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

// May a consumer take `stepDb` back in the loud direction? Requires
//     headroom >= step + sampling-bias bound + caller margin
// The step because N dB less attenuation raises the ADC input N dB; the bias
// because the gated peak errs quiet, the dangerous direction. Absent licenses
// nothing (fall back, don't treat as "no room"); NearRail goes through the
// same arithmetic.
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
