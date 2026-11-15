#pragma once

#include <QString>
#include <QHash>
#include <QSet>
#include <QVector>

namespace AetherSDR {

struct QsoRecord;

enum class DxccStatus {
    NewDxcc,   // entity never worked
    NewBand,   // entity worked but not on this band
    NewMode,   // entity worked on this band but not this mode group
    Worked,    // already worked on this band + mode group
    Unknown,   // DXCC not resolved (no cty.dat match)
};

// ---------------------------------------------------------------------------
// DxccWorkedStatus
//
// Fast O(1) lookup of worked status from a loaded ADIF log.
// Data: QHash<primaryPrefix, QHash<band, QSet<modeGroup>>>
// ---------------------------------------------------------------------------
class DxccWorkedStatus {
public:
    void load(const QVector<QsoRecord>& records);
    void clear();

    // Query worked status given the resolved DXCC primary prefix, band, and
    // normalised mode group (CW / PHONE / DATA).
    DxccStatus query(const QString& primaryPrefix,
                     const QString& band,
                     const QString& modeGroup) const;

    int entityCount() const { return m_worked.size(); }
    int totalQsos()   const { return m_totalQsos; }

private:
    // primaryPrefix -> band -> set<modeGroup>
    QHash<QString, QHash<QString, QSet<QString>>> m_worked;
    int m_totalQsos{0};
};

} // namespace AetherSDR
