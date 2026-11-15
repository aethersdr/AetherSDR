#pragma once

#include "BandDefs.h"

#include <QObject>
#include <QMap>
#include <QString>

namespace AetherSDR {

// Snapshot of all per-band settings that should be saved/restored.
struct BandSnapshot {
    // Slice settings
    double  frequencyMhz{0.0};
    QString mode;
    QString rxAntenna;
    int     filterLow{0};
    int     filterHigh{0};
    QString agcMode;
    int     agcThreshold{0};

    // Panadapter/display settings (client-side only — bandwidth and center
    // are radio-authoritative and must NOT be saved/restored here)
    int    rfGain{0};
    bool   wnbOn{false};
    int    wnbLevel{50};
    float  minDbm{-130.0f};
    float  maxDbm{-40.0f};
    float  spectrumFrac{0.40f};

    bool isValid() const { return frequencyMhz > 0.0; }
};

// Manages per-band settings persistence.
// Stores a BandSnapshot for each band the user has visited.
class BandSettings : public QObject {
    Q_OBJECT

public:
    explicit BandSettings(QObject* parent = nullptr);

    // Frequency → band name lookup ("20m", "GEN", etc.)
    // Checks amateur band ranges; returns "GEN" if no match.
    static QString bandForFrequency(double freqMhz);

    // Band name → BandDef. Returns kGenBand if not found.
    static const BandDef& bandDef(const QString& name);

    // Save/load a single band's state in memory.
    void saveBandState(const QString& bandName, const BandSnapshot& snap);
    BandSnapshot loadBandState(const QString& bandName) const;
    bool hasSavedState(const QString& bandName) const;

    // Persist all in-memory state to disk (deprecated — see issue #9).
    void saveToFile() const;
    // Load from disk into memory.
    void loadFromFile();

    // Current band tracking.
    QString currentBand() const { return m_currentBand; }
    void setCurrentBand(const QString& band) { m_currentBand = band; }

private:
    QMap<QString, BandSnapshot> m_bandStates;
    QString m_currentBand;
};

} // namespace AetherSDR
