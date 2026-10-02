#pragma once

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QString>

namespace AetherSDR {

struct NetEntry;

// Recurrence math for scheduled nets; QtCore only, exhaustively unit-tested.
// RFC 5545 RRULE subset:
//   FREQ=DAILY[;INTERVAL=n]
//   FREQ=WEEKLY[;INTERVAL=n];BYDAY=MO,TU,...  (BYDAY defaults to startDate's day)
//   FREQ=MONTHLY[;INTERVAL=n];BYDAY=nXX       (one ordinal weekday, n in -1..5,
//                                              -1 = last)
// INTERVAL > 1 is phased from startDate (DTSTART); without one, treated as 1.
namespace NetRecurrence {

struct ParsedRule {
    enum class Freq { Invalid, Daily, Weekly, Monthly };

    Freq      freq{Freq::Invalid};
    int       interval{1};
    QList<int> weekdays;        // Qt day-of-week (1=Mon..7=Sun) for WEEKLY BYDAY
    int       monthlyOrdinal{0}; // -1 (last) .. 5 for MONTHLY BYDAY=nXX; 0 = unset
    int       monthlyWeekday{0}; // Qt day-of-week (1..7) for MONTHLY

    bool isValid() const { return freq != Freq::Invalid; }
};

// Parse an RRULE string into the supported subset. Unknown FREQ or malformed
// input yields an invalid rule (isValid() == false).
ParsedRule parseRule(const QString& rrule);

// Compute the next occurrence START strictly after `afterUtc`, honoring the
// local wall-clock `timeOfDay` ("HH:MM") in IANA `timezone`, DST-correct.
// `startDate` (ISO "yyyy-MM-dd", may be null) anchors INTERVAL phasing and
// provides the default weekday for a BYDAY-less weekly rule.
//
// Returns a QDateTime carrying the entry timezone (use toMSecsSinceEpoch() /
// toUTC() for comparisons). Returns an invalid QDateTime if the rule is
// invalid or no occurrence is found within the search horizon.
QDateTime nextOccurrence(const QString& rrule,
                         const QString& timeOfDay,
                         const QString& timezone,
                         const QDate& startDate,
                         const QDateTime& afterUtc);

// Convenience overload operating on a NetEntry.
QDateTime nextOccurrence(const NetEntry& entry, const QDateTime& afterUtc);

// Human-readable, friendly summary of a rule for the UI confirmation chip,
// e.g. "Weekly on Tuesday", "Every 2 weeks on Mon, Wed", "Monthly on the
// first Sunday", "Daily". Returns an empty string for an invalid rule.
QString describeRule(const QString& rrule);

} // namespace NetRecurrence

} // namespace AetherSDR
