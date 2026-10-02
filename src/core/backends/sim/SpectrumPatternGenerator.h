#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

namespace AetherSDR {

// SpectrumPatternGenerator — the demo-mode spectrum engine (RFC #4288 Phase 2).
// Produces one FFT line of per-bin dBm (the SpectrumWidget::updateSpectrum()
// payload) as a pure, deterministic function of (pattern, time, geometry), so
// golden captures repeat. Clean-room port of nigelfenton/flex-sim (GPL-3.0)
// pat_* generators against flex-sim/PROTOCOL.md; "noisy" patterns use
// flex-sim's integer hash (_hash01) keyed on (frame, bin), not a live RNG.
class SpectrumPatternGenerator {
public:
    enum class Pattern {
        NoiseFloor,     // flat baseline at the floor
        Ramp,           // whole-band level sweeps floor -> max over RampPeriod
        CalTones,       // fixed calibration tones at -100/-80/-60/-40 dBm
        SweptCarrier,   // a single carrier sweeping left -> right
        TwoTone,        // two equal tones symmetric about center (IMD ruler)
        Comb,           // evenly spaced tones across the span
        NoiseBand,      // a band of seeded (reproducible) random noise at center
    };

    struct Geometry {
        int    bins = 1024;        // FFT bin count (== SpectrumWidget line length)
        double minDbm = -140.0;    // pan floor / display range low
        double maxDbm = -20.0;     // pan ceiling / display range high
        double spanHz = 250000.0;  // pan span, for Hz->bin math (two-tone, band width)
        double signalWidthHz = 10000.0;  // synthesized carrier/band width
    };

    // One FFT line at wall-clock t (seconds) and frame index (drives the seeded
    // RNG). Length == geometry.bins; every value is clamped into [minDbm,maxDbm].
    static QVector<float> generate(Pattern pattern, const Geometry& geometry,
                                   double t, quint64 frameIndex);

    // Pattern <-> stable lowercase name (for settings + a UI picker later).
    static QString     name(Pattern pattern);
    static Pattern     fromName(const QString& name, bool* ok = nullptr);
    static QStringList allNames();

private:
    // flex-sim's _hash01: a deterministic [0,1) value from an integer key, so
    // "random" patterns reproduce frame-for-frame (golden captures repeat).
    static double hash01(quint32 key);
};

}  // namespace AetherSDR
