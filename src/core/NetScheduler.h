#pragma once

#include "core/NetSchedulePlanner.h"
#include "models/NetEntry.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

namespace AetherSDR {

// Drives net reminders from a single recompute-and-rearm timer (the standard
// calendar-engine pattern): always arm one timer at the soonest pending
// reminder, fire it, advance, re-arm. The sleep is capped so the schedule
// self-heals across laptop suspend, wall-clock changes, and DST transitions —
// on every wake it recomputes against the current time rather than trusting a
// far-future delta. All scheduling logic delegates to the pure
// nextReminderAcross() planner; this class only owns the timer and clock.
class NetScheduler : public QObject {
    Q_OBJECT

public:
    explicit NetScheduler(QObject* parent = nullptr);

    // Replace the active entry set and immediately recompute the next reminder.
    void setEntries(const QList<NetEntry>& entries);
    QList<NetEntry> entries() const { return m_entries; }

    // The currently-armed reminder, for diagnostics / UI "Next: ..." display.
    PendingReminder nextPending() const { return m_armed; }

Q_SIGNALS:
    // Emitted when a net's reminder fires. `occurrenceUtc` is when the net
    // starts; the lead-time has already elapsed-to.
    void reminderDue(const AetherSDR::NetEntry& entry, const QDateTime& occurrenceUtc);

private:
    void rearm();
    void onTimeout();
    QDateTime nowUtc() const;

    QList<NetEntry> m_entries;
    QTimer m_timer;
    PendingReminder m_armed;
    QSet<QString> m_firedKeys;   // "id|occurrenceMsSinceEpoch" — double-fire guard
};

} // namespace AetherSDR
