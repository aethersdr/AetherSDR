#include "core/NetSchedulePlanner.h"

#include "core/NetRecurrence.h"
#include "models/NetEntry.h"

namespace AetherSDR {

PendingReminder nextReminderAcross(const QList<NetEntry>& entries, const QDateTime& afterUtc)
{
    PendingReminder best;
    const QDateTime afterUtcNorm = afterUtc.toUTC();

    for (int i = 0; i < entries.size(); ++i) {
        const NetEntry& entry = entries.at(i);
        if (!entry.enabled)
            continue;

        const int lead = entry.reminderLeadMinutes > 0 ? entry.reminderLeadMinutes : 0;
        const qint64 leadSecs = static_cast<qint64>(lead) * 60;

        // Walk occurrences forward until this entry yields a reminder instant
        // strictly after `afterUtc`. The lead-time can push a reminder before
        // its occurrence into the past, so the soonest future *occurrence* does
        // not always give the soonest future *reminder* — advance until it does.
        QDateTime cursorUtc = afterUtcNorm;
        for (int guard = 0; guard < 64; ++guard) {
            const QDateTime occ = NetRecurrence::nextOccurrence(entry, cursorUtc);
            if (!occ.isValid())
                break;
            const QDateTime occUtc = occ.toUTC();
            const QDateTime reminderUtc = occUtc.addSecs(-leadSecs);
            if (reminderUtc > afterUtcNorm) {
                if (!best.isValid() || reminderUtc < best.reminderUtc) {
                    best.entryIndex = i;
                    best.occurrenceUtc = occUtc;
                    best.reminderUtc = reminderUtc;
                }
                break;
            }
            // Reminder already in the past; skip this occurrence and look further.
            cursorUtc = occUtc;
        }
    }

    return best;
}

} // namespace AetherSDR
