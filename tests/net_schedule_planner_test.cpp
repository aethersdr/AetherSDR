#include "core/NetSchedulePlanner.h"

#include "models/NetEntry.h"

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QTime>
#include <QTimeZone>

#include <iostream>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

QDateTime utc(int y, int mo, int d, int h, int mi)
{
    return QDateTime(QDate(y, mo, d), QTime(h, mi), QTimeZone("UTC"));
}

NetEntry weeklyNet(const QString& id, int qtDow, const QString& time, int lead)
{
    static const char* codes[] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
    NetEntry e;
    e.id = id;
    e.name = id;
    e.rrule = QString("FREQ=WEEKLY;BYDAY=%1").arg(codes[qtDow - 1]);
    e.timeOfDay = time;
    e.timezone = "Etc/UTC";
    e.reminderLeadMinutes = lead;
    return e;
}

} // namespace

int main()
{
    bool ok = true;

    // Reference: Mon 2026-06-01 00:00 UTC.
    const QDateTime now = utc(2026, 6, 1, 0, 0);

    // --- picks the soonest reminder across entries ----------------------
    {
        // Tuesday net 20:00 (lead 10) → reminder Tue Jun 2 19:50Z.
        // Wednesday net 12:00 (lead 5) → reminder Wed Jun 3 11:55Z.
        // Tuesday's reminder is sooner.
        QList<NetEntry> nets = {weeklyNet("tue", 2, "20:00", 10),
                                weeklyNet("wed", 3, "12:00", 5)};
        const PendingReminder p = nextReminderAcross(nets, now);
        ok &= expect(p.isValid() && p.entryIndex == 0, "soonest reminder is the Tuesday net");
        ok &= expect(p.occurrenceUtc == utc(2026, 6, 2, 20, 0), "occurrence instant correct");
        ok &= expect(p.reminderUtc == utc(2026, 6, 2, 19, 50), "reminder = occurrence - lead");
    }

    // --- a long lead-time can reorder which fires first -----------------
    {
        // Wednesday net 12:00 with a huge 1500-min (25h) lead → reminder
        // Tue Jun 2 11:00Z, which beats the Tuesday net's 19:50Z reminder.
        QList<NetEntry> nets = {weeklyNet("tue", 2, "20:00", 10),
                                weeklyNet("wed", 3, "12:00", 1500)};
        const PendingReminder p = nextReminderAcross(nets, now);
        ok &= expect(p.isValid() && p.entryIndex == 1,
                     "the longer lead-time fires first despite a later occurrence");
        ok &= expect(p.reminderUtc == utc(2026, 6, 2, 11, 0), "long-lead reminder instant correct");
    }

    // --- disabled entries are skipped ----------------------------------
    {
        QList<NetEntry> nets = {weeklyNet("tue", 2, "20:00", 10),
                                weeklyNet("wed", 3, "12:00", 5)};
        nets[0].enabled = false;
        const PendingReminder p = nextReminderAcross(nets, now);
        ok &= expect(p.isValid() && p.entryIndex == 1, "disabled net is skipped");
    }

    // --- advances past an occurrence whose reminder already elapsed -----
    {
        // Net at 20:00 with lead 10 → reminder 19:50. If "now" is 19:55 (after
        // the reminder but before the net), the next *reminder* is next week.
        QList<NetEntry> nets = {weeklyNet("tue", 2, "20:00", 10)};
        const PendingReminder p = nextReminderAcross(nets, utc(2026, 6, 2, 19, 55));
        ok &= expect(p.isValid() && p.reminderUtc == utc(2026, 6, 9, 19, 50),
                     "past-reminder occurrence is skipped to next week");
    }

    // --- nothing pending ------------------------------------------------
    {
        QList<NetEntry> empty;
        ok &= expect(!nextReminderAcross(empty, now).isValid(), "empty list yields no reminder");

        QList<NetEntry> invalid = {weeklyNet("x", 2, "20:00", 10)};
        invalid[0].rrule = "FREQ=YEARLY";
        ok &= expect(!nextReminderAcross(invalid, now).isValid(),
                     "invalid rule yields no reminder");
    }

    return ok ? 0 : 1;
}
