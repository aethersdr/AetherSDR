#pragma once

#include "gui/SplitQsySettings.h"

#include <QVector>
#include <QtGlobal>

#include <cmath>

namespace AetherSDR {

class PendingSliceFrequencyEchoes {
public:
    static constexpr int kCapacity = 32;
    static constexpr qint64 kLifetimeMs = 2000;
    static constexpr double kFrequencyToleranceMhz = 0.000001;

    void record(int sliceId, double frequencyMhz, qint64 nowMs)
    {
        discardExpired(nowMs);
        if (m_entries.size() == kCapacity) {
            m_entries.removeFirst();
        }
        m_entries.append({sliceId, frequencyMhz, nowMs + kLifetimeMs});
    }

    bool consume(int sliceId, double frequencyMhz, qint64 nowMs)
    {
        for (int index = 0; index < m_entries.size();) {
            const Entry& entry = m_entries.at(index);
            if (entry.expiresAtMs <= nowMs) {
                m_entries.removeAt(index);
                continue;
            }
            if (sliceId == entry.sliceId
                && std::abs(frequencyMhz - entry.frequencyMhz)
                <= kFrequencyToleranceMhz) {
                m_entries.removeAt(index);
                return true;
            }
            ++index;
        }
        return false;
    }

    void clear()
    {
        m_entries.clear();
    }

    bool empty() const
    {
        return m_entries.isEmpty();
    }

private:
    struct Entry {
        int sliceId;
        double frequencyMhz;
        qint64 expiresAtMs;
    };

    void discardExpired(qint64 nowMs)
    {
        for (int index = 0; index < m_entries.size();) {
            if (m_entries.at(index).expiresAtMs <= nowMs) {
                m_entries.removeAt(index);
            } else {
                ++index;
            }
        }
    }

    QVector<Entry> m_entries;
};

inline bool shouldCloseSplitOnQsyObservation(
    const SplitQsySettings& settings, bool splitActive, bool rxSlice,
    double frequencyMhz, double& referenceFrequencyMhz)
{
    if (!splitActive || !rxSlice) {
        return false;
    }
    if (!std::isfinite(frequencyMhz) || frequencyMhz <= 0.0) {
        return false;
    }
    if (!std::isfinite(referenceFrequencyMhz)
        || referenceFrequencyMhz <= 0.0) {
        referenceFrequencyMhz = frequencyMhz;
        return false;
    }
    const bool closeSplit = settings.closeSplitOnQsy
        && shouldCloseSplitOnQsy(
            settings, true, true, frequencyMhz, referenceFrequencyMhz);
    if (!closeSplit) {
        referenceFrequencyMhz = frequencyMhz;
    }
    return closeSplit;
}

} // namespace AetherSDR