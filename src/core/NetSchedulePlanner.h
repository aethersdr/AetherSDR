#pragma once

#include <QDateTime>
#include <QList>

namespace AetherSDR {

struct NetEntry;

// Pure planning helper for the net scheduler: given the current entry list and
// "now", find the single soonest reminder to fire. Headless and deterministic
// (no timers, no clock of its own) so the scheduling decision can be unit-tested
// directly. The QObject NetScheduler is a thin timer wrapper around this.
struct PendingReminder {
    int       entryIndex{-1};      // index into the entries list
    QDateTime occurrenceUtc;       // when the net starts (UTC)
    QDateTime reminderUtc;         // when to alert = occurrence - leadMinutes (UTC)

    bool isValid() const { return entryIndex >= 0; }
};

// Earliest reminder strictly after `afterUtc` across all enabled entries.
// Disabled entries, entries with invalid rules, and entries whose reminder
// instant is not after `afterUtc` are skipped. Returns an invalid
// PendingReminder when nothing is pending.
PendingReminder nextReminderAcross(const QList<NetEntry>& entries, const QDateTime& afterUtc);

} // namespace AetherSDR
