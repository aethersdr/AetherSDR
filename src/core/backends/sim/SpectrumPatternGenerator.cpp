#include "core/backends/sim/SpectrumPatternGenerator.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {
// flex-sim pattern constants (see flex-sim/flex_sim.py).
constexpr double kRampPeriodS  = 10.0;
constexpr double kSweepPeriodS = 8.0;
constexpr int    kCombTones    = 8;
constexpr double kTwoToneSpacingHz = 2000.0;

// The live sim exposes floor/signal-level sliders; flex-sim's headless defaults
// derive them from the display range: a visible floor 10 dB up from the bottom,
// and a signal peak 5 dB below the top.
double floorDbm(const SpectrumPatternGenerator::Geometry& g)  { return g.minDbm + 10.0; }
double signalDbm(const SpectrumPatternGenerator::Geometry& g) { return g.maxDbm - 5.0; }

int centerBin(const SpectrumPatternGenerator::Geometry& g) { return g.bins / 2; }

// Half-width, in bins, of the synthesized carrier/band.
int signalHalfBins(const SpectrumPatternGenerator::Geometry& g)
{
    if (g.spanHz <= 0.0) return 0;
    const double binsPerHz = static_cast<double>(g.bins) / g.spanHz;
    return std::max(0, static_cast<int>(g.signalWidthHz * binsPerHz / 2.0));
}

QVector<float> flat(const SpectrumPatternGenerator::Geometry& g, double dbm)
{
    return QVector<float>(g.bins, static_cast<float>(dbm));
}
}  // namespace

double SpectrumPatternGenerator::hash01(quint32 n)
{
    // flex-sim _hash01, verbatim behavior (unsigned 32-bit throughout).
    n = (n ^ 61u) ^ (n >> 16);
    n = (n + (n << 3));
    n ^= (n >> 4);
    n = n * 0x27d4eb2du;
    n ^= (n >> 15);
    return static_cast<double>(n) / 4294967296.0;
}

QVector<float> SpectrumPatternGenerator::generate(Pattern pattern,
                                                  const Geometry& geometry,
                                                  double t, quint64 frameIndex)
{
    const Geometry& g = geometry;
    const double floorL = floorDbm(g);
    const double sigL = signalDbm(g);
    QVector<float> out;

    switch (pattern) {
    case Pattern::NoiseFloor:
        out = flat(g, floorL);
        break;

    case Pattern::Ramp: {
        const double frac = std::fmod(t, kRampPeriodS) / kRampPeriodS;
        out = flat(g, g.minDbm + frac * (g.maxDbm - g.minDbm));
        break;
    }

    case Pattern::CalTones: {
        out = flat(g, floorL);
        const std::pair<double, double> tones[] = {
            {0.2, -100.0}, {0.4, -80.0}, {0.6, -60.0}, {0.8, -40.0}};
        for (const auto& [frac, dbm] : tones) {
            const int b = static_cast<int>(frac * g.bins);
            for (int d = -1; d <= 1; ++d) {
                const int idx = b + d;
                if (idx >= 0 && idx < g.bins) {
                    out[idx] = std::max(out[idx], static_cast<float>(dbm));
                }
            }
        }
        break;
    }

    case Pattern::SweptCarrier: {
        out = flat(g, floorL);
        const double frac = std::fmod(t, kSweepPeriodS) / kSweepPeriodS;
        const int c = static_cast<int>(frac * (g.bins - 1));
        const int half = signalHalfBins(g);
        for (int d = -half; d <= half; ++d) {
            const int b = c + d;
            if (b >= 0 && b < g.bins) {
                out[b] = static_cast<float>(sigL);
            }
        }
        break;
    }

    case Pattern::TwoTone: {
        out = flat(g, floorL);
        // Two equal tones symmetric about the VFO, kTwoToneSpacingHz apart.
        const double binsPerHz =
            g.spanHz > 0.0 ? static_cast<double>(g.bins) / g.spanHz : 0.0;
        const int halfBins =
            std::max(0, static_cast<int>(kTwoToneSpacingHz / 2.0 * binsPerHz));
        const int center = centerBin(g);
        for (const int sign : {-1, +1}) {
            const int b = center + sign * halfBins;
            if (b >= 0 && b < g.bins) {
                out[b] = static_cast<float>(sigL);
            }
        }
        break;
    }

    case Pattern::Comb: {
        out = flat(g, floorL);
        const int step = std::max(1, g.bins / kCombTones);
        for (int b = step / 2; b < g.bins; b += step) {
            out[b] = static_cast<float>(sigL - 10.0);
        }
        break;
    }

    case Pattern::NoiseBand: {
        out = flat(g, floorL);
        const int center = centerBin(g);
        const int half = signalHalfBins(g);
        const int lo = std::max(0, center - half);
        const int hi = std::min(g.bins - 1, center + half);
        for (int b = lo; b <= hi; ++b) {
            // Seeded per (frame, bin) so the texture is reproducible.
            const quint32 key =
                static_cast<quint32>(frameIndex) * 1000003u + static_cast<quint32>(b);
            const double r = hash01(key);
            out[b] = static_cast<float>(sigL - r * 12.0);
        }
        break;
    }
    }

    // Clamp every bin into the display range (matches the real decode path).
    const float lo = static_cast<float>(g.minDbm);
    const float hi = static_cast<float>(g.maxDbm);
    for (float& v : out) {
        v = std::clamp(v, lo, hi);
    }
    return out;
}

QString SpectrumPatternGenerator::name(Pattern pattern)
{
    switch (pattern) {
    case Pattern::NoiseFloor:   return QStringLiteral("noise_floor");
    case Pattern::Ramp:         return QStringLiteral("ramp");
    case Pattern::CalTones:     return QStringLiteral("cal_tones");
    case Pattern::SweptCarrier: return QStringLiteral("swept_carrier");
    case Pattern::TwoTone:      return QStringLiteral("two_tone");
    case Pattern::Comb:         return QStringLiteral("comb");
    case Pattern::NoiseBand:    return QStringLiteral("noise_band");
    }
    return QStringLiteral("noise_floor");
}

SpectrumPatternGenerator::Pattern
SpectrumPatternGenerator::fromName(const QString& name, bool* ok)
{
    if (ok) *ok = true;
    for (const Pattern p : {Pattern::NoiseFloor, Pattern::Ramp, Pattern::CalTones,
                            Pattern::SweptCarrier, Pattern::TwoTone, Pattern::Comb,
                            Pattern::NoiseBand}) {
        if (SpectrumPatternGenerator::name(p) == name) {
            return p;
        }
    }
    if (ok) *ok = false;
    return Pattern::NoiseFloor;   // safe default on an unknown name
}

QStringList SpectrumPatternGenerator::allNames()
{
    return {QStringLiteral("noise_floor"), QStringLiteral("ramp"),
            QStringLiteral("cal_tones"),   QStringLiteral("swept_carrier"),
            QStringLiteral("two_tone"),    QStringLiteral("comb"),
            QStringLiteral("noise_band")};
}

}  // namespace AetherSDR
