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
    bool   hetReject       = false;    // opt-in: pull a cut inboard of a narrow
                                       // strong interferer sitting near the edge
};

// measureOccupiedRegion: single-signal occupied-bandwidth edge finder (RFC
// #3878). Not VoiceSignalDetector::detectVoiceSignals(), which chunks wide
// regions (~2.7 kHz) and would fragment ESSB. Anchors on the carrier and fits the
// contiguous energy on the signal's side. Edges are relative, never calibrated:
//   * INNER (low-cut): first bin clearing floorCurve + kEnvGateDb; must stay
//     floor-relative (peak-relative floats with syllable level).
//   * OUTER (high-cut): innermost of the floor return, a separate stronger lobe,
//     and the splatter cap (referenceDbm - kSplatterDownDb).
//   * Each edge snaps to a steep transition if one exists, else stays at the
//     level/floor extent.
//  binsDbm        full-pan FFT magnitudes (dBm)
//  centerMhz/bandwidthMhz  the pan span
//  carrierMhz     the slice's suppressed-carrier frequency
//  mode           "USB" or "LSB" (selects the energy side)
//  noiseFloorDbm  SpectrumWidget's rolling floor (<= -500 = unknown); seeds and
//                 cross-checks the floor curve
//  avgEnv         in/out per-slice temporal envelope average (persistent;
//                 reinit on geometry change). Not a per-bin peak-hold.
OccupiedRegion measureOccupiedRegion(const QVector<float>& binsDbm,
                                     double centerMhz, double bandwidthMhz,
                                     double carrierMhz, const QString& mode,
                                     float noiseFloorDbm,
                                     QVector<float>& avgEnv,
                                     const OccupiedRegionParams& params = {});

} // namespace AetherSDR
