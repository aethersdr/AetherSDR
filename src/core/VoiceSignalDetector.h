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

// Find contiguous regions ≥1.8 kHz wide and ≥6 dB above the noise floor in
// FFT bins (dBm). Regions are capped at 2.7 kHz; overflow becomes a second
// marker only if it has its own qualifying peak.
//   voiceRangesMhz: limit the scan to these {lowMhz, highMhz} ranges (band-plan
//     voice segments); empty = whole pan.
//   rollingNoiseFloorDbm: used if > -500, else the per-frame 10th percentile.
//   sliceMode: "USB"/"LSB" overrides the energy-asymmetry heuristic; empty
//     uses the heuristic (e.g. AM/FM pans).
QVector<DetectedVoiceSignal> detectVoiceSignals(
    const QVector<float>& binsDbm,
    double centerMhz,
    double bandwidthMhz,
    const QVector<QPair<double, double>>& voiceRangesMhz = {},
    float rollingNoiseFloorDbm = -1000.0f,
    const QString& sliceMode = {});

// Format a peak-dBm value as an S-meter label, rounded UP to the next unit.
// Scale: S9 = -73 dBm, 6 dB/S-unit.  Examples: -85 → "S8", -63 → "S9+10".
QString sLabel(float dbm);

} // namespace AetherSDR
