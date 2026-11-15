#include "DxccWorkedStatus.h"
#include "AdifParser.h"

namespace AetherSDR {

void DxccWorkedStatus::load(const QVector<QsoRecord>& records)
{
    m_worked.clear();
    m_totalQsos = 0;
    for (const auto& r : records) {
        if (r.dxccPrefix.isEmpty() || r.band.isEmpty() || r.modeGroup.isEmpty())
            continue;
        m_worked[r.dxccPrefix][r.band].insert(r.modeGroup);
        ++m_totalQsos;
    }
}

void DxccWorkedStatus::clear()
{
    m_worked.clear();
    m_totalQsos = 0;
}

DxccStatus DxccWorkedStatus::query(const QString& primaryPrefix,
                                   const QString& band,
                                   const QString& modeGroup) const
{
    if (primaryPrefix.isEmpty()) return DxccStatus::Unknown;

    auto entityIt = m_worked.find(primaryPrefix);
    if (entityIt == m_worked.end())
        return DxccStatus::NewDxcc;

    auto bandIt = entityIt->find(band);
    if (bandIt == entityIt->end())
        return DxccStatus::NewBand;

    if (!bandIt->contains(modeGroup))
        return DxccStatus::NewMode;

    return DxccStatus::Worked;
}

} // namespace AetherSDR
