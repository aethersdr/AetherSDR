#pragma once

#include <QHash>
#include <QString>

#include <optional>
#include <utility>

namespace AetherSDR {

// One panadapter's writes deferred during the profile-load hold (#4142).
// Absent optional = the user never asked for that field while the hold was
// armed; it must not be written at flush time.
struct PanWrites {
    std::optional<QString> bandKey;
    std::optional<double> centerMhz;
    std::optional<double> bandwidthMhz;

    bool isEmpty() const
    {
        return !bandKey.has_value() && !centerMhz.has_value()
            && !bandwidthMhz.has_value();
    }
};

// Deferred pan-write store for the profile-load hold (#4142): sendCmd()
// suppresses profile-owned pan writes while the hold is armed
// (isProfileOwnedRadioStateWrite) and the user-intent part waits here.
//  - Coalescing is field-wise per pan, last write wins per field; a later
//    center defer must never erase a queued bandwidth.
//  - supersede*() erases a field group that just reached the wire directly, so
//    a stale pending value cannot override it.
//  - cancel() voids a pan entry (pan died) and returns the voided fields for
//    logging; entries with every field superseded are already erased.
// Owned by RadioModel; Qt6::Core-only so profile_load_command_test links it.
class ProfileLoadPanWriteQueue {
public:
    void deferBand(const QString& panId, const QString& bandKey)
    {
        m_pending[panId].bandKey = bandKey;
    }

    void deferCenter(const QString& panId, double centerMhz)
    {
        m_pending[panId].centerMhz = centerMhz;
    }

    void deferCenterBandwidth(const QString& panId, double centerMhz,
                              double bandwidthMhz)
    {
        PanWrites& entry = m_pending[panId];
        entry.centerMhz = centerMhz;
        entry.bandwidthMhz = bandwidthMhz;
    }

    void deferBandwidth(const QString& panId, double bandwidthMhz)
    {
        m_pending[panId].bandwidthMhz = bandwidthMhz;
    }

    void supersedeCenter(const QString& panId)
    {
        const auto it = m_pending.find(panId);
        if (it == m_pending.end()) {
            return;
        }
        it->centerMhz.reset();
        if (it->isEmpty()) {
            m_pending.erase(it);
        }
    }

    void supersedeBandwidth(const QString& panId)
    {
        const auto it = m_pending.find(panId);
        if (it == m_pending.end()) {
            return;
        }
        it->bandwidthMhz.reset();
        if (it->isEmpty()) {
            m_pending.erase(it);
        }
    }

    void supersedeCenterBandwidth(const QString& panId)
    {
        const auto it = m_pending.find(panId);
        if (it == m_pending.end()) {
            return;
        }
        it->centerMhz.reset();
        it->bandwidthMhz.reset();
        if (it->isEmpty()) {
            m_pending.erase(it);
        }
    }

    void supersedeBand(const QString& panId)
    {
        const auto it = m_pending.find(panId);
        if (it == m_pending.end()) {
            return;
        }
        it->bandKey.reset();
        if (it->isEmpty()) {
            m_pending.erase(it);
        }
    }

    // The pan died (removed, ownership lost, disconnect). Returns the voided
    // entry so the caller can log the destroyed fields, or nullopt when
    // nothing was pending.
    std::optional<PanWrites> cancel(const QString& panId)
    {
        const auto it = m_pending.find(panId);
        if (it == m_pending.end()) {
            return std::nullopt;
        }
        PanWrites voided = std::move(*it);
        m_pending.erase(it);
        return voided;
    }

    std::optional<double> pendingCenter(const QString& panId) const
    {
        const auto it = m_pending.constFind(panId);
        return it == m_pending.constEnd() ? std::nullopt : it->centerMhz;
    }

    std::optional<double> pendingBandwidth(const QString& panId) const
    {
        const auto it = m_pending.constFind(panId);
        return it == m_pending.constEnd() ? std::nullopt : it->bandwidthMhz;
    }

    std::optional<QString> pendingBand(const QString& panId) const
    {
        const auto it = m_pending.constFind(panId);
        return it == m_pending.constEnd() ? std::nullopt : it->bandKey;
    }

    QHash<QString, PanWrites> takeAll()
    {
        return std::exchange(m_pending, {});
    }

    bool isEmpty() const { return m_pending.isEmpty(); }
    int size() const { return m_pending.size(); }
    void clear() { m_pending.clear(); }

private:
    // Invariant: no stored entry is all-empty — supersede/cancel erase them.
    QHash<QString, PanWrites> m_pending;
};

} // namespace AetherSDR
