#pragma once

#include <QVector>
#include <QString>
#include <QPair>

namespace AetherSDR {

// A voice-bandwidth SSB signal detected in a single FFT frame.
struct DetectedVoiceSignal {
    double  freqMhz;   // carrier edge frequency (left for USB, right for LSB)
    float   peakDbm;   // loudest bin within the region
    QString mode;      // "USB" or "LSB"
    double  widthHz;   // detected bandwidth (useful for QRM notch width)
};

// Returns true if a band-plan segment label designates a voice allocation
// (PHONE, SSB, USB, AM, FM/RPT).
bool isVoiceSegmentLabel(const QString& label);

// Scan FFT bins (in dBm) for contiguous regions ≥1.8 kHz wide and ≥6 dB above
// the stable noise floor.  Each detected region is capped at 2.7 kHz (one SSB
// channel); wider regions are split and the overflow is emitted as a second
// marker only if it also has a qualifying peak.
//
// voiceRangesMhz: if non-empty, only scan bins whose frequency falls inside one
// of these {lowMhz, highMhz} ranges (derived from the active band plan's voice
// segments).  Pass an empty vector to scan the full pan bandwidth.
//
// rollingNoiseFloorDbm: caller-supplied stable noise floor (e.g. EMA across
// recent frames).  When > -500 dBm it is used directly; otherwise the function
// falls back to the per-frame 10th-percentile estimate.
//
// sliceMode: "USB" or "LSB" from the active slice.  When supplied, it overrides
// the internal energy-asymmetry heuristic so markers always match the operator's
// current mode.  Pass an empty string to use the heuristic (e.g. for AM/FM pans).
//
// softEdgeDb: when >= 0, refines the carrier-side edge of voice-width regions
// by walking outward from the 6 dB region boundary using this softer
// (noise floor + softEdgeDb) threshold, until 3 consecutive bins fall below
// the threshold.  This pulls the marker closer to the carrier than the main
// region threshold allows, at the cost of a small amount of extra compute per
// detected region.  Pass < 0 to disable the refinement (the historical
// behaviour: marker sits exactly at the 6 dB region edge).
//
// Returns one entry per detected signal (primary + optional secondary per region).
QVector<DetectedVoiceSignal> detectVoiceSignals(
    const QVector<float>& binsDbm,
    double centerMhz,
    double bandwidthMhz,
    const QVector<QPair<double, double>>& voiceRangesMhz = {},
    float rollingNoiseFloorDbm = -1000.0f,
    const QString& sliceMode = {},
    float softEdgeDb = -1.0f);

// Format a peak-dBm value as an S-meter label, rounded UP to the next unit.
// Scale: S9 = -73 dBm, 6 dB/S-unit.  Examples: -85 → "S8", -63 → "S9+10".
QString sLabel(float dbm);

} // namespace AetherSDR
