#pragma once

#include <QChar>
#include <QString>
#include <QTime>

namespace AetherSDR {

struct DxSpot {
    QString spotterCall;    // W3LPL
    double  freqMhz{0.0};  // 14.025 (converted from kHz)
    QString dxCall;         // JA1ABC
    QString comment;        // "CW big signal"
    QTime   utcTime;        // 18:24 UTC
    QString source;         // "Cluster", "RBN", "WSJT-X"
    QString color;          // #AARRGGBB for radio spot color (optional)
    int     snr{0};         // signal-to-noise ratio (dB): WSJT-X decodes, skimmer/RBN reports
    bool    hasSnr{false};  // snr carries a reading (0 dB is a real one)
    int     lifetimeSec{0}; // 0 = use source default from AppSettings

    // GoCluster's fixed tail (empty/null on every other server).
    QString dxGrid;         // 4-char DX grid; lowercase = derived by the cluster
    QChar   confidence;     // call confidence: ? S P V C B
    QChar   pathGlyph;      // path prediction: > = < - #
};

} // namespace AetherSDR
