#include "ImdAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR::txlin {
namespace {

double dbToAmplitude(double db)
{
    return std::pow(10.0, db / 20.0);
}

// Median of the bins that belong to neither a fundamental nor a product skirt.
//
// MEDIAN, not mean: the guard set inevitably still contains the odd real
// signal — a spur, a birdie, a stray carrier — and a mean would let one of them
// lift the reported floor by tens of dB, which would then suppress every
// genuine product as "limited by noise floor". The median is indifferent to a
// minority of outliers, which is exactly the robustness this needs.
double estimateNoiseFloorDb(const std::vector<double>& power,
                            const std::vector<int>& excludeCentres,
                            int excludeHalfBins)
{
    const int n = int(power.size());
    std::vector<bool> masked(std::size_t(n), false);
    for (int centre : excludeCentres) {
        const int lo = std::max(0, centre - excludeHalfBins);
        const int hi = std::min(n - 1, centre + excludeHalfBins);
        for (int k = lo; k <= hi; ++k) {
            masked[std::size_t(k)] = true;
        }
    }

    // Trim the outer 5% of the capture band. A DAX IQ stream is decimated by
    // the radio's own filters, whose transition band lives right at the edges;
    // including it would measure the filter skirt and call it the noise floor.
    const int edge = std::max(1, n / 20);
    std::vector<double> keep;
    keep.reserve(std::size_t(n));
    for (int k = edge; k < n - edge; ++k) {
        if (!masked[std::size_t(k)] && power[std::size_t(k)] > 0.0) {
            keep.push_back(power[std::size_t(k)]);
        }
    }
    if (keep.empty()) {
        return -300.0;
    }
    const std::size_t mid = keep.size() / 2;
    std::nth_element(keep.begin(), keep.begin() + std::ptrdiff_t(mid), keep.end());
    return 10.0 * std::log10(keep[mid]);
}

}  // namespace

std::optional<BandPower> integrateBand(const TxSpectrumAnalyzer& analyzer,
                                       double sampleRateHz,
                                       double lowHz, double highHz,
                                       double toneRefDb, double pepDb)
{
    const std::vector<double>& power = analyzer.powerSpectrum();
    const int n = int(power.size());
    if (n < 4 || sampleRateHz <= 0.0 || highHz <= lowHz) {
        return std::nullopt;
    }

    const double bw = TxSpectrumAnalyzer::binHz(sampleRateHz, n);
    const double nyq = sampleRateHz / 2.0;
    // Refuse rather than truncate. A band clipped at the capture edge
    // integrates less energy than the caller asked for and reports it as though
    // it were the whole band — a silently optimistic ACPR figure, which is the
    // worst kind of wrong for this instrument.
    if (lowHz < -nyq || highHz > nyq) {
        return std::nullopt;
    }

    const int loBin = std::clamp(int(std::floor(lowHz / bw)) + n / 2, 0, n - 1);
    const int hiBin = std::clamp(int(std::ceil(highHz / bw)) + n / 2, 0, n - 1);
    if (hiBin <= loBin) {
        return std::nullopt;
    }

    double sum = 0.0;
    for (int k = loBin; k <= hiBin; ++k) {
        sum += power[std::size_t(k)];
    }
    const double enbw = analyzer.window().enbwBins;
    const double p = (enbw > 0.0) ? (sum / enbw) : sum;
    if (p <= 0.0) {
        return std::nullopt;
    }

    BandPower out;
    out.lowHz = lowHz;
    out.highHz = highHz;
    out.powerDb = 10.0 * std::log10(p);
    out.dbcPerTone = out.powerDb - toneRefDb;
    out.dbcPep = out.powerDb - pepDb;
    return out;
}

LinearityResult measureTwoTone(const TxSpectrumAnalyzer& analyzer,
                               double sampleRateHz,
                               const ImdConfig& config,
                               bool clipped)
{
    LinearityResult r;
    r.sampleRateHz = sampleRateHz;
    r.fftSize = analyzer.fftSize();
    r.averages = analyzer.averages();
    r.clipped = clipped;

    const std::vector<double>& power = analyzer.powerSpectrum();
    const int n = int(power.size());
    if (n < 16 || analyzer.averages() == 0 || sampleRateHz <= 0.0) {
        r.invalidReason = "No averaged spectrum available";
        return r;
    }

    r.binHz = TxSpectrumAnalyzer::binHz(sampleRateHz, n);
    analyzer.toDb(r.spectrumDb);

    const int lobe = std::max(1, analyzer.window().mainLobeHalfBins);
    const int centre = n / 2;

    // ── Fundamentals ────────────────────────────────────────────────────────
    //
    // Search the whole band except the DC notch, take the strongest bin, then
    // blank its entire main lobe before searching again. Blanking the main lobe
    // rather than a fixed span is what stops the second "tone" being the first
    // tone's own skirt, which is the failure this loop exists to prevent.
    std::vector<double> search = power;
    for (int k = centre - config.dcNotchBins; k <= centre + config.dcNotchBins; ++k) {
        if (k >= 0 && k < n) {
            search[std::size_t(k)] = 0.0;
        }
    }

    auto takePeak = [&](InterpolatedPeak& out) {
        out = interpolatePeak(search, 1, n - 2, sampleRateHz);
        if (!out.valid) {
            return;
        }
        const int bin = std::clamp(int(std::lround(out.freqHz / r.binHz)) + centre, 0, n - 1);
        for (int k = std::max(0, bin - lobe); k <= std::min(n - 1, bin + lobe); ++k) {
            search[std::size_t(k)] = 0.0;
        }
    };

    InterpolatedPeak p1;
    InterpolatedPeak p2;
    takePeak(p1);
    takePeak(p2);
    if (!p1.valid || !p2.valid) {
        r.invalidReason = "Could not resolve two tones in the capture";
        return r;
    }

    // Order them low-to-high so f1 < f2 and the product formulae below read as
    // written rather than depending on which happened to be stronger.
    if (p2.freqHz < p1.freqHz) {
        std::swap(p1, p2);
    }

    r.tone1 = TonePeak{true, p1.freqHz, p1.powerDb};
    r.tone2 = TonePeak{true, p2.freqHz, p2.powerDb};
    r.toneSpacingHz = p2.freqHz - p1.freqHz;
    r.toneImbalanceDb = p2.powerDb - p1.powerDb;

    // Two peaks closer together than the window can separate are one tone read
    // twice, whatever the blanking did.
    if (r.toneSpacingHz < 2.0 * double(lobe) * r.binHz) {
        r.invalidReason = "Tones are closer than the analysis window can resolve";
        return r;
    }

    // ── PEP ─────────────────────────────────────────────────────────────────
    //
    // From the MEASURED amplitudes, not from tone power + 6.02 dB. For a
    // balanced pair the two agree exactly (that is the §9 reference-convention
    // test); for an imbalanced pair the envelope peak is A1 + A2 and the
    // idealisation would be wrong in the direction that flatters the radio.
    const double a1 = dbToAmplitude(r.tone1.powerDb);
    const double a2 = dbToAmplitude(r.tone2.powerDb);
    r.pepDb = 20.0 * std::log10(a1 + a2);

    // The single-tone reference. With an imbalanced pair there is no one "the
    // tone", so use the mean power of the two — it degrades to A^2 exactly when
    // they are balanced, and it is the choice that keeps the reported
    // per-tone/PEP pair self-consistent.
    const double toneRefDb = 10.0 * std::log10(0.5 * (a1 * a1 + a2 * a2));

    // ── Products ────────────────────────────────────────────────────────────
    const double nyq = sampleRateHz / 2.0;
    std::vector<int> guardCentres{
        std::clamp(int(std::lround(r.tone1.freqHz / r.binHz)) + centre, 0, n - 1),
        std::clamp(int(std::lround(r.tone2.freqHz / r.binHz)) + centre, 0, n - 1),
        centre,  // DC / LO leakage is not noise either
    };

    struct Candidate {
        int order;
        bool upper;
        double freqHz;
    };
    std::vector<Candidate> candidates;
    for (int order = 3; order <= config.maxOrder; order += 2) {
        // 2f1-f2 is d beyond f1; 3f1-2f2 is 2d beyond; generally
        // ((order+1)/2)*f1 - ((order-1)/2)*f2 = f1 - ((order-1)/2)*d.
        const double steps = double((order - 1) / 2);
        candidates.push_back({order, false, r.tone1.freqHz - steps * r.toneSpacingHz});
        candidates.push_back({order, true,  r.tone2.freqHz + steps * r.toneSpacingHz});
    }

    for (const Candidate& c : candidates) {
        // A product predicted outside the capture band is not a missing
        // measurement, it is an unmeasurable one — skip it silently rather than
        // reporting the band-edge filter skirt as an IMD level.
        if (std::abs(c.freqHz) > nyq - double(lobe) * r.binHz) {
            continue;
        }
        const int predicted = std::clamp(
            int(std::lround(c.freqHz / r.binHz)) + centre, 1, n - 2);
        // Search only the main lobe around the predicted frequency. Wider would
        // let a neighbouring spur be reported as the product; narrower would
        // miss it when the tone estimates carry a fraction of a bin of error.
        const InterpolatedPeak pk =
            interpolatePeak(power, predicted - lobe, predicted + lobe, sampleRateHz);
        if (!pk.valid) {
            continue;
        }

        ImdProduct prod;
        prod.order = c.order;
        prod.upper = c.upper;
        prod.freqHz = pk.freqHz;
        prod.powerDb = pk.powerDb;
        prod.dbcPerTone = pk.powerDb - toneRefDb;
        prod.dbcPep = pk.powerDb - r.pepDb;
        r.products.push_back(prod);

        guardCentres.push_back(
            std::clamp(int(std::lround(pk.freqHz / r.binHz)) + centre, 0, n - 1));
    }

    // ── Instrument noise floor ──────────────────────────────────────────────
    //
    // Computed AFTER the products are located so their skirts are excluded from
    // the guard set. Excluding twice the main lobe leaves the window's own
    // sidelobe structure out of the estimate too, which would otherwise make
    // the floor look band-dependent rather than being a property of the
    // instrument.
    r.noiseFloorDb = estimateNoiseFloorDb(power, guardCentres, 2 * lobe);
    for (ImdProduct& prod : r.products) {
        prod.limitedByNoiseFloor =
            (prod.powerDb < r.noiseFloorDb + config.noiseFloorMarginDb);
    }

    // ── Shoulder and ACPR ───────────────────────────────────────────────────
    if (config.measureShoulder && config.shoulderBandwidthHz > 0.0) {
        const double half = config.shoulderBandwidthHz / 2.0;
        r.shoulderLow = integrateBand(analyzer, sampleRateHz,
                                      r.tone1.freqHz - config.shoulderOffsetHz - half,
                                      r.tone1.freqHz - config.shoulderOffsetHz + half,
                                      toneRefDb, r.pepDb);
        r.shoulderHigh = integrateBand(analyzer, sampleRateHz,
                                       r.tone2.freqHz + config.shoulderOffsetHz - half,
                                       r.tone2.freqHz + config.shoulderOffsetHz + half,
                                       toneRefDb, r.pepDb);
    }
    if (config.measureAcpr && config.acprBandwidthHz > 0.0) {
        // Occupied channel: the tone pair widened by half the spacing at each
        // end, which is where an SSB two-tone's own energy stops and the
        // distortion skirt starts.
        const double chanLow = r.tone1.freqHz - r.toneSpacingHz / 2.0;
        const double chanHigh = r.tone2.freqHz + r.toneSpacingHz / 2.0;
        const double lowEdge = chanLow - config.acprGapHz;
        const double highEdge = chanHigh + config.acprGapHz;
        r.acprLow = integrateBand(analyzer, sampleRateHz,
                                  lowEdge - config.acprBandwidthHz, lowEdge,
                                  toneRefDb, r.pepDb);
        r.acprHigh = integrateBand(analyzer, sampleRateHz,
                                   highEdge, highEdge + config.acprBandwidthHz,
                                   toneRefDb, r.pepDb);
    }

    r.valid = true;
    return r;
}

}  // namespace AetherSDR::txlin
