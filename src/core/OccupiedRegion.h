#pragma once

#include <QString>
#include <QVector>

namespace AetherSDR {

// Result of a single-signal occupied-bandwidth measurement, in *audio*
// magnitudes (Hz from the suppressed carrier, always low < high). The caller
// maps these to signed filter offsets per mode (USB: lo=+low, hi=+high;
// LSB: lo=-high, hi=-low).
//
// This header is deliberately free of any SliceModel / Q_OBJECT dependency so
// the measurement core can be unit-tested against Qt6::Core alone
// (tests/adaptive_filter_test.cpp). The temporal pipeline that consumes it
// lives in AdaptiveFilterEngine.{h,cpp}.
struct OccupiedRegion {
    bool   valid{false};            // a confident measurement was obtained
    int    lowHz{0};                // low-cut: nearest-carrier edge of the energy
    int    highHz{0};               // high-cut: far edge of the energy
    float  peakDbm{-1000.0f};       // envelope peak (the loud voice level)
    float  referenceDbm{-1000.0f};  // robust in-band reference (high percentile
                                    // of the kept extent) — the relative anchor
                                    // for the outer caps and soft edges
    float  floorDbm{-1000.0f};      // the scalar noise floor the measurement
                                    // actually used (caller-supplied, or the
                                    // local fallback when the caller sent the
                                    // sentinel) — lets the engine key low-SNR
                                    // behaviour on peakDbm - floorDbm without
                                    // guessing which floor was in effect
};

// Operator-tunable knobs for measureOccupiedRegion (mapped from the SliceModel
// Minimum-SNR / Splatter-rejection settings). Defaults are the "Normal" presets.
struct OccupiedRegionParams {
    float  minPeakDb       = 9.0f;     // presence gate: in-band peak must clear the
                                       // noise floor by at least this (Min SNR)
    float  splatterDownDb  = 25.0f;    // outer-cap level below the in-band reference
    double splatterGuardHz = 3200.0;   // splatter cap engages only past this extent
};

// measureOccupiedRegion — a single-signal occupied-bandwidth edge-finder
// (RFC #3878). This is deliberately NOT VoiceSignalDetector::detectVoiceSignals(),
// which is a band-scan marker detector that splits wide regions into ~2.7 kHz
// chunks and would fragment a wide ESSB signal. Here we anchor on the slice
// carrier, scan only a local window on the signal's energy side, and fit the
// contiguous run of energy.
//
// Edge logic, in relative terms (never absolute calibration):
//  * INNER (low-cut) edge is floor-relative — the first bin clearing
//    floorCurve + kEnvGateDb. This must stay floor-relative: a peak-relative
//    inner threshold floated narrower on loud syllables and wider on quiet ones.
//  * OUTER (high-cut) edge is bounded by the inner of: the floor return, a
//    SEPARATE stronger lobe (rebound), and a splatter cap placed relative to the
//    in-band reference (referenceDbm - kSplatterDownDb), so a slowly-decaying
//    dirty tail is excluded rather than chased to the floor.
//  * Each edge is then refined per-edge: snapped to a steep transition when a
//    clear cliff exists (modern DSP rigs), else left at the level/floor extent
//    (soft analog roll-offs).
//
//  binsDbm        full-pan FFT magnitudes (dBm)
//  centerMhz/bandwidthMhz   the pan span
//  carrierMhz     the slice's suppressed-carrier frequency
//  mode           "USB" or "LSB" (selects the energy side)
//  noiseFloorDbm  rolling floor from SpectrumWidget (sentinel <= -500 => unknown);
//                 used to seed and cross-check the per-frequency floor curve
//  avgEnv         in/out per-slice temporal average (video averaging): a
//                 per-offset EMA of the envelope that reduces frame-to-frame
//                 noise before the edge threshold — stabilises edges on
//                 weak/medium signals. Persistent per-slice; reinit on geometry
//                 change. (NOT a peak-hold — a per-bin peak-hold accumulated and
//                 inflated the width over time; QSB is ridden by the bounded
//                 edge peak-hold instead.)
OccupiedRegion measureOccupiedRegion(const QVector<float>& binsDbm,
                                     double centerMhz, double bandwidthMhz,
                                     double carrierMhz, const QString& mode,
                                     float noiseFloorDbm,
                                     QVector<float>& avgEnv,
                                     const OccupiedRegionParams& params = {});

} // namespace AetherSDR
