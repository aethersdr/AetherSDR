#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <algorithm>
#include <cmath>
#include <optional>

#include "backends/MeterDef.h"

namespace AetherSDR {

// Bounded, read-only observation of already-converted meter samples. The caller
// supplies the same epoch-ms clock as MeterModel. No transport or TX ownership.
class MeterObservationWindow {
public:
    void start(qint64 now, int durationMs)
    {
        m_start = now;
        m_end = now + durationMs;
        m_observedUntil = now;
        m_entries.clear();
    }

    void observe(const MeterDef& def, qint64 sampleAt, float value, qint64 now)
    {
        const qint64 until = std::clamp(now, m_start, m_end);
        m_observedUntil = std::max(m_observedUntil, until);
        Entry& entry = m_entries[def.index];
        entry.def = def;
        // Before replacing the old timestamp, account for the age immediately
        // before this arrival. A fresh sample must not erase a scheduler stall.
        if (entry.lastSampleAt > 0) {
            const qint64 oldUntil = sampleAt > entry.lastSampleAt
                ? std::min(until, sampleAt) : until;
            entry.maxAgeMs = std::max(entry.maxAgeMs, oldUntil - entry.lastSampleAt);
        }
        if (sampleAt > 0 && sampleAt <= until) {
            entry.maxAgeMs = std::max(entry.maxAgeMs, until - sampleAt);
            // Epoch milliseconds are not a unique arrival ID: queued samples
            // may share a timestamp. Reobserving a cached value is harmless
            // for a maximum, while discarding an equal-timestamp peak is not.
            if (sampleAt >= m_start && std::isfinite(value)) {
                entry.peak = entry.peak ? std::max(*entry.peak, value) : value;
                if (!entry.firstSampleAt) {
                    entry.firstSampleAt = sampleAt;
                }
            }
            entry.lastSampleAt = sampleAt;
        }
    }

    [[nodiscard]] bool expired(qint64 now) const { return now >= m_end; }

    [[nodiscard]] QJsonObject snapshot() const
    {
        QJsonArray meters;
        for (const Entry& e : m_entries) {
            meters.append(QJsonObject{
                {QStringLiteral("index"), e.def.index},
                {QStringLiteral("source"), e.def.source},
                {QStringLiteral("sourceIndex"), e.def.sourceIndex},
                {QStringLiteral("name"), e.def.name},
                {QStringLiteral("unit"), e.def.unit},
                {QStringLiteral("maxAgeMs"), e.maxAgeMs >= 0 ? QJsonValue(e.maxAgeMs) : QJsonValue()},
                {QStringLiteral("receivedInWindow"), e.firstSampleAt.has_value()},
                {QStringLiteral("firstSampleDelayMs"), e.firstSampleAt
                    ? QJsonValue(*e.firstSampleAt - m_start) : QJsonValue()},
                {QStringLiteral("peakInWindow"), e.peak ? QJsonValue(*e.peak) : QJsonValue()},
            });
        }
        return {{QStringLiteral("startedAtMs"), m_start},
                {QStringLiteral("durationMs"), m_end - m_start},
                {QStringLiteral("observedMs"), m_observedUntil - m_start},
                {QStringLiteral("meters"), meters}};
    }

private:
    struct Entry {
        MeterDef def;
        qint64 lastSampleAt{0};
        qint64 maxAgeMs{-1};
        std::optional<qint64> firstSampleAt;
        std::optional<float> peak;
    };
    qint64 m_start{0};
    qint64 m_end{0};
    qint64 m_observedUntil{0};
    QMap<int, Entry> m_entries;
};

} // namespace AetherSDR
