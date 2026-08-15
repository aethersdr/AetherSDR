#pragma once

#include "TxLinearityTypes.h"
#include "TxSpectrumAnalyzer.h"

namespace AetherSDR::txlin {

// Two-tone intermodulation measurement over an averaged spectrum.
//
// For two tones at f1 and f2 with spacing d = f2 - f1, the odd-order products
// fall symmetrically outside the pair:
//
//   order | lower      | upper
//   ------+------------+-----------
//     3   | 2f1 - f2   | 2f2 - f1      (d beyond each tone)
//     5   | 3f1 - 2f2  | 3f2 - 2f1     (2d beyond)
//     7   | 4f1 - 3f2  | 4f2 - 3f1     (3d beyond)
//
// Even-order products land at DC and at 2f, far outside the capture band on a
// baseband IQ capture of an SSB signal, so they are not measured here — their
// absence is a property of the signal path, not an omission.
struct ImdConfig {
    // Highest odd order to report. 7 is the practical limit for a feedback path
    // with ~80 dB of dynamic range; beyond that the products are below any
    // realistic instrument floor and reporting them invites reading noise as a
    // measurement.
    int maxOrder = 7;

    // A product peak must clear the measured instrument noise floor by this
    // much for the number to be trusted. Below it the product is flagged
    // limitedByNoiseFloor and the UI says so rather than printing the figure as
    // though it were a measurement. 6 dB is the conventional "you can see it
    // but do not quote it" threshold; the value at the floor itself is a coin
    // flip between signal and noise.
    double noiseFloorMarginDb = 6.0;

    // Beyond this the two tones are unbalanced enough that the symmetric-IMD
    // interpretation and the exact 6.02 dB PEP relation both stop holding.
    double toneImbalanceWarnDb = 0.5;

    // Ignore this many bins either side of exact DC when searching for the
    // fundamentals. A direct-conversion feedback path puts an LO-leakage spike
    // at DC that is not a tone; without this it can win the "two strongest
    // peaks" search outright. Deliberately narrow — a genuine tone only a few
    // bins from DC is unmeasurable anyway, since its window skirt wraps.
    int dcNotchBins = 3;

    // Shoulder measurement: a band `shoulderBandwidthHz` wide, centred
    // `shoulderOffsetHz` outside each fundamental. The shoulder is where the
    // IMD skirt plateaus, so it characterises the splatter an adjacent operator
    // actually hears — which the discrete product levels do not.
    double shoulderOffsetHz = 5000.0;
    double shoulderBandwidthHz = 1000.0;

    // ACPR: a band `acprBandwidthHz` wide, starting `acprGapHz` outside the
    // occupied channel. The occupied channel is taken as the tone pair widened
    // by half the tone spacing at each end.
    double acprGapHz = 1000.0;
    double acprBandwidthHz = 3000.0;

    // Set false to skip shoulder/ACPR when the capture bandwidth cannot hold
    // the bands (the analyzer also refuses on its own and leaves the optional
    // results disengaged rather than reporting a truncated integration).
    bool measureShoulder = true;
    bool measureAcpr = true;
};

// Run the full reference-free (Phase 1) measurement.
//
// `clipped` is the capture path's overload verdict, carried through so the
// result records that the numbers came from a saturated capture even though
// the arithmetic itself cannot tell.
//
// Returns a result with valid == false and a populated invalidReason whenever
// the spectrum cannot support a measurement — no averages, fewer than two
// resolvable tones, tones too close to separate, products falling outside the
// capture band. Refusing is a first-class outcome; this function never
// fabricates a figure to have something to display.
LinearityResult measureTwoTone(const TxSpectrumAnalyzer& analyzer,
                               double sampleRateHz,
                               const ImdConfig& config,
                               bool clipped);

// Integrated power over [lowHz, highHz] of a DC-centred power spectrum,
// expressed against both reference conventions.
//
// Exposed because the UI marks these bands on the plot and must integrate
// exactly what it draws. sum(|S[k]|^2)/enbwBins is correct for a discrete tone
// and for a noise-like band alike — see AnalysisWindow for why.
std::optional<BandPower> integrateBand(const TxSpectrumAnalyzer& analyzer,
                                       double sampleRateHz,
                                       double lowHz, double highHz,
                                       double toneRefDb, double pepDb);

}  // namespace AetherSDR::txlin
