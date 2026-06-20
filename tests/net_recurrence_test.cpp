#include "core/NetRecurrence.h"

#include <QDate>
#include <QDateTime>
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

// UTC instant builder that avoids Qt 6.5+ QTimeZone::utc() (CI Qt is pre-6.5).
QDateTime utc(int y, int mo, int d, int h, int mi)
{
    return QDateTime(QDate(y, mo, d), QTime(h, mi), QTimeZone("UTC"));
}

} // namespace

int main()
{
    bool ok = true;

    // --- parseRule ------------------------------------------------------
    {
        const auto r = NetRecurrence::parseRule("FREQ=WEEKLY;BYDAY=TU");
        ok &= expect(r.isValid() && r.freq == NetRecurrence::ParsedRule::Freq::Weekly,
                     "weekly rule parses");
        ok &= expect(r.weekdays.size() == 1 && r.weekdays.first() == 2,
                     "weekly BYDAY=TU maps to Qt day 2");
    }
    {
        const auto r = NetRecurrence::parseRule("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE,FR");
        ok &= expect(r.interval == 2 && r.weekdays.size() == 3,
                     "weekly interval + multi BYDAY parse");
    }
    {
        const auto r = NetRecurrence::parseRule("FREQ=MONTHLY;BYDAY=1SU");
        ok &= expect(r.isValid() && r.monthlyOrdinal == 1 && r.monthlyWeekday == 7,
                     "monthly first-Sunday parses");
    }
    {
        const auto r = NetRecurrence::parseRule("FREQ=MONTHLY;BYDAY=-1WE");
        ok &= expect(r.isValid() && r.monthlyOrdinal == -1 && r.monthlyWeekday == 3,
                     "monthly last-Wednesday parses");
    }
    {
        ok &= expect(!NetRecurrence::parseRule("FREQ=YEARLY").isValid(),
                     "unsupported FREQ is invalid");
        ok &= expect(!NetRecurrence::parseRule("garbage").isValid(),
                     "garbage rule is invalid");
        ok &= expect(!NetRecurrence::parseRule("FREQ=MONTHLY").isValid(),
                     "monthly without BYDAY is invalid");
    }

    // --- weekly next occurrence ----------------------------------------
    {
        // Tuesdays 20:00 UTC. After 2026-06-01 00:00Z → Tue Jun 2 20:00Z.
        const QDateTime occ = NetRecurrence::nextOccurrence(
            "FREQ=WEEKLY;BYDAY=TU", "20:00", "Etc/UTC", QDate(), utc(2026, 6, 1, 0, 0));
        ok &= expect(occ.toUTC() == utc(2026, 6, 2, 20, 0),
                     "weekly Tuesday picks the next Tuesday at 20:00 UTC");
    }
    {
        // Same day but earlier than the time → still fires today.
        const QDateTime occ = NetRecurrence::nextOccurrence(
            "FREQ=WEEKLY;BYDAY=TU", "20:00", "Etc/UTC", QDate(), utc(2026, 6, 2, 10, 0));
        ok &= expect(occ.toUTC() == utc(2026, 6, 2, 20, 0),
                     "same-day occurrence later than now is returned");
    }
    {
        // Strictly after: at exactly the occurrence, return the next week.
        const QDateTime occ = NetRecurrence::nextOccurrence(
            "FREQ=WEEKLY;BYDAY=TU", "20:00", "Etc/UTC", QDate(), utc(2026, 6, 2, 20, 0));
        ok &= expect(occ.toUTC() == utc(2026, 6, 9, 20, 0),
                     "occurrence is strictly after the reference instant");
    }

    // --- weekly INTERVAL phase (needs startDate anchor) -----------------
    {
        // Every 2 weeks on Tuesday, anchored Tue Jun 2 → Jun 2, 16, 30 (NOT Jun 9).
        const QDateTime occ = NetRecurrence::nextOccurrence(
            "FREQ=WEEKLY;INTERVAL=2;BYDAY=TU", "20:00", "Etc/UTC", QDate(2026, 6, 2),
            utc(2026, 6, 2, 20, 0));
        ok &= expect(occ.toUTC() == utc(2026, 6, 16, 20, 0),
                     "biweekly skips the off-week using the startDate anchor");
    }

    // --- monthly nth weekday -------------------------------------------
    {
        // First Sunday of the month. After Jun 1 2026 → Sun Jun 7.
        const QDateTime occ = NetRecurrence::nextOccurrence(
            "FREQ=MONTHLY;BYDAY=1SU", "09:00", "Etc/UTC", QDate(), utc(2026, 6, 1, 0, 0));
        ok &= expect(occ.toUTC() == utc(2026, 6, 7, 9, 0),
                     "monthly first-Sunday resolves to Jun 7");
    }
    {
        // Last Wednesday of June 2026 is Jun 24.
        const QDateTime occ = NetRecurrence::nextOccurrence(
            "FREQ=MONTHLY;BYDAY=-1WE", "09:00", "Etc/UTC", QDate(), utc(2026, 6, 1, 0, 0));
        ok &= expect(occ.toUTC() == utc(2026, 6, 24, 9, 0),
                     "monthly last-Wednesday resolves to Jun 24");
    }

    // --- daily interval -------------------------------------------------
    {
        // Every 3 days anchored Jun 1 → Jun 1, 4, 7. After Jun 2 → Jun 4.
        const QDateTime occ = NetRecurrence::nextOccurrence(
            "FREQ=DAILY;INTERVAL=3", "12:00", "Etc/UTC", QDate(2026, 6, 1),
            utc(2026, 6, 2, 0, 0));
        ok &= expect(occ.toUTC() == utc(2026, 6, 4, 12, 0),
                     "every-3-days honors the anchor phase");
    }

    // --- DST correctness (America/Chicago, 20:00 local stays 20:00) -----
    {
        // Winter (CST = UTC-6): Tue Feb 3 2026, 20:00 CST = Feb 4 02:00 UTC.
        const QDateTime winter = NetRecurrence::nextOccurrence(
            "FREQ=WEEKLY;BYDAY=TU", "20:00", "America/Chicago", QDate(), utc(2026, 2, 1, 0, 0));
        const QDateTime winterLocal = winter.toTimeZone(QTimeZone("America/Chicago"));
        ok &= expect(winterLocal.time() == QTime(20, 0),
                     "winter net is 20:00 local (CST)");
        ok &= expect(winter.toUTC() == utc(2026, 2, 4, 2, 0),
                     "20:00 CST equals 02:00 UTC");

        // Summer (CDT = UTC-5): Tue Jul 7 2026, 20:00 CDT = Jul 8 01:00 UTC.
        // Use noon UTC as the reference so the local search day is Jul 1 (a
        // midnight-UTC reference would land on Jun 30 evening Chicago time).
        const QDateTime summer = NetRecurrence::nextOccurrence(
            "FREQ=WEEKLY;BYDAY=TU", "20:00", "America/Chicago", QDate(), utc(2026, 7, 1, 12, 0));
        const QDateTime summerLocal = summer.toTimeZone(QTimeZone("America/Chicago"));
        ok &= expect(summerLocal.time() == QTime(20, 0),
                     "summer net is still 20:00 local (CDT)");
        ok &= expect(summer.toUTC() == utc(2026, 7, 8, 1, 0),
                     "20:00 CDT equals 01:00 UTC — DST offset applied");
    }

    // --- one-time ("Once" / Repeat = Never) -----------------------------
    {
        // Empty rule = single event at startDate + timeOfDay.
        const QDateTime occ = NetRecurrence::nextOccurrence(
            "", "20:00", "Etc/UTC", QDate(2026, 6, 20), utc(2026, 6, 19, 0, 0));
        ok &= expect(occ.toUTC() == utc(2026, 6, 20, 20, 0),
                     "one-time net fires at its start date/time");

        // Already past → no occurrence.
        const QDateTime gone = NetRecurrence::nextOccurrence(
            "", "20:00", "Etc/UTC", QDate(2026, 6, 20), utc(2026, 6, 21, 0, 0));
        ok &= expect(!gone.isValid(), "one-time net does not recur once past");

        // Same-day-later still fires (the same-day testing case).
        const QDateTime sameDay = NetRecurrence::nextOccurrence(
            "", "20:00", "Etc/UTC", QDate(2026, 6, 19), utc(2026, 6, 19, 10, 0));
        ok &= expect(sameDay.toUTC() == utc(2026, 6, 19, 20, 0),
                     "one-time net later the same day still fires");

        // No date → nothing.
        ok &= expect(!NetRecurrence::nextOccurrence("", "20:00", "Etc/UTC", QDate(),
                                                    utc(2026, 6, 19, 0, 0)).isValid(),
                     "one-time net with no date yields nothing");
    }

    // --- describeRule ---------------------------------------------------
    {
        ok &= expect(NetRecurrence::describeRule("FREQ=WEEKLY;BYDAY=TU") == "Weekly on Tuesday",
                     "weekly description");
        ok &= expect(NetRecurrence::describeRule("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE")
                         == "Every 2 weeks on Monday, Wednesday",
                     "biweekly multi-day description");
        ok &= expect(NetRecurrence::describeRule("FREQ=MONTHLY;BYDAY=1SU")
                         == "Monthly on the first Sunday",
                     "monthly description");
        ok &= expect(NetRecurrence::describeRule("FREQ=DAILY") == "Daily", "daily description");
        ok &= expect(NetRecurrence::describeRule("") == "Once", "empty rule describes as Once");
    }

    return ok ? 0 : 1;
}
