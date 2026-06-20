#include "core/NetRecurrence.h"

#include "models/NetEntry.h"

#include <QStringList>
#include <QTime>
#include <QTimeZone>

namespace AetherSDR {
namespace NetRecurrence {

namespace {

// Search horizon: large enough to cross any supported interval (monthly with a
// modest INTERVAL, or a sparse weekly BYDAY) yet bounded so a rule that can
// never fire terminates instead of looping forever.
constexpr int kSearchHorizonDays = 800;

// Map an RRULE two-letter weekday code to Qt's day-of-week (1=Mon..7=Sun).
int weekdayFromCode(const QString& code)
{
    static const QStringList kCodes = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
    const int idx = kCodes.indexOf(code.trimmed().toUpper());
    return idx < 0 ? 0 : idx + 1;
}

QString weekdayLongName(int qtDow)
{
    static const QStringList kNames = {"Monday",   "Tuesday", "Wednesday", "Thursday",
                                       "Friday",   "Saturday", "Sunday"};
    if (qtDow < 1 || qtDow > 7)
        return {};
    return kNames.at(qtDow - 1);
}

QString ordinalName(int ordinal)
{
    switch (ordinal) {
    case 1:  return "first";
    case 2:  return "second";
    case 3:  return "third";
    case 4:  return "fourth";
    case 5:  return "fifth";
    case -1: return "last";
    default: return {};
    }
}

// Date of the nth `weekday` (Qt 1..7) within `year`/`month`. ordinal>0 counts
// from the start; ordinal==-1 is the last such weekday. Returns a null QDate if
// that ordinal does not exist in the month (e.g. a 5th Friday that month lacks).
QDate nthWeekdayOfMonth(int year, int month, int weekday, int ordinal)
{
    if (ordinal == -1) {
        const QDate last(year, month, QDate(year, month, 1).daysInMonth());
        const int delta = (last.dayOfWeek() - weekday + 7) % 7;
        return last.addDays(-delta);
    }
    if (ordinal < 1 || ordinal > 5)
        return {};
    const QDate first(year, month, 1);
    const int firstDelta = (weekday - first.dayOfWeek() + 7) % 7;
    const QDate candidate = first.addDays(firstDelta + (ordinal - 1) * 7);
    if (candidate.month() != month)
        return {};
    return candidate;
}

// Whole calendar months between two dates (a may be after or before b).
int monthsBetween(const QDate& a, const QDate& b)
{
    return (b.year() - a.year()) * 12 + (b.month() - a.month());
}

// Monday-anchored week index since an epoch, so weekly INTERVAL phase is stable
// regardless of which weekday is being tested.
qint64 mondayWeekIndex(const QDate& d)
{
    const QDate monday = d.addDays(-(d.dayOfWeek() - 1));
    return monday.toJulianDay() / 7;
}

// Build a zoned QDateTime for a local wall-clock time, handling a spring-forward
// gap (where that wall time doesn't exist) portably across Qt versions — Qt 6.7's
// QDateTime::TransitionResolution isn't available on the 6.4 CI floor. On a gap
// the plain constructor returns an invalid datetime; nudge forward one hour to
// land just past the transition. (A net at 20:00 essentially never hits the
// ~02:00 DST gap, but we must not crash or silently skip if it does.) On a
// fall-back overlap the constructor picks deterministically, which is fine since
// the scheduler always recomputes.
QDateTime zonedDateTime(const QDate& date, const QTime& time, const QTimeZone& tz)
{
    QDateTime dt(date, time, tz);
    if (!dt.isValid())
        dt = QDateTime(date, time.addSecs(3600), tz);
    return dt;
}

bool matchesRule(const QDate& date, const ParsedRule& rule, const QDate& startDate)
{
    const bool haveAnchor = startDate.isValid();

    switch (rule.freq) {
    case ParsedRule::Freq::Daily: {
        if (rule.interval <= 1 || !haveAnchor)
            return true;
        const qint64 days = startDate.daysTo(date);
        return days >= 0 && (days % rule.interval) == 0;
    }
    case ParsedRule::Freq::Weekly: {
        QList<int> days = rule.weekdays;
        if (days.isEmpty() && haveAnchor)
            days << startDate.dayOfWeek();
        if (!days.contains(date.dayOfWeek()))
            return false;
        if (rule.interval <= 1 || !haveAnchor)
            return true;
        const qint64 weeks = mondayWeekIndex(date) - mondayWeekIndex(startDate);
        return weeks >= 0 && (weeks % rule.interval) == 0;
    }
    case ParsedRule::Freq::Monthly: {
        if (rule.monthlyWeekday == 0 || rule.monthlyOrdinal == 0)
            return false;
        const QDate target =
            nthWeekdayOfMonth(date.year(), date.month(), rule.monthlyWeekday, rule.monthlyOrdinal);
        if (target != date)
            return false;
        if (rule.interval <= 1 || !haveAnchor)
            return true;
        const int months = monthsBetween(startDate, date);
        return months >= 0 && (months % rule.interval) == 0;
    }
    case ParsedRule::Freq::Invalid:
        break;
    }
    return false;
}

} // namespace

ParsedRule parseRule(const QString& rrule)
{
    ParsedRule rule;
    const QStringList parts = rrule.trimmed().split(';', Qt::SkipEmptyParts);
    QString byDay;

    for (const QString& part : parts) {
        const int eq = part.indexOf('=');
        if (eq < 0)
            continue;
        const QString key = part.left(eq).trimmed().toUpper();
        const QString val = part.mid(eq + 1).trimmed();

        if (key == "FREQ") {
            const QString f = val.toUpper();
            if (f == "DAILY")
                rule.freq = ParsedRule::Freq::Daily;
            else if (f == "WEEKLY")
                rule.freq = ParsedRule::Freq::Weekly;
            else if (f == "MONTHLY")
                rule.freq = ParsedRule::Freq::Monthly;
        } else if (key == "INTERVAL") {
            bool ok = false;
            const int n = val.toInt(&ok);
            if (ok && n > 0)
                rule.interval = n;
        } else if (key == "BYDAY") {
            byDay = val;
        }
    }

    if (!rule.isValid())
        return rule;

    if (rule.freq == ParsedRule::Freq::Monthly) {
        // BYDAY for MONTHLY is a single ordinal weekday, e.g. "1SU" or "-1WE".
        const QString token = byDay.trimmed().toUpper();
        int idx = 0;
        bool negative = false;
        if (token.startsWith('-')) {
            negative = true;
            idx = 1;
        } else if (token.startsWith('+')) {
            idx = 1;
        }
        int digitsEnd = idx;
        while (digitsEnd < token.size() && token.at(digitsEnd).isDigit())
            ++digitsEnd;
        if (digitsEnd > idx) {
            int ord = token.mid(idx, digitsEnd - idx).toInt();
            if (negative)
                ord = -ord;
            rule.monthlyOrdinal = ord;
            rule.monthlyWeekday = weekdayFromCode(token.mid(digitsEnd));
        }
        if (rule.monthlyWeekday == 0 || rule.monthlyOrdinal == 0)
            rule.freq = ParsedRule::Freq::Invalid;
        return rule;
    }

    if (rule.freq == ParsedRule::Freq::Weekly && !byDay.isEmpty()) {
        const QStringList codes = byDay.split(',', Qt::SkipEmptyParts);
        for (const QString& code : codes) {
            const int dow = weekdayFromCode(code);
            if (dow != 0 && !rule.weekdays.contains(dow))
                rule.weekdays << dow;
        }
    }

    return rule;
}

QDateTime nextOccurrence(const QString& rrule,
                         const QString& timeOfDay,
                         const QString& timezone,
                         const QDate& startDate,
                         const QDateTime& afterUtc)
{
    const QTime time = QTime::fromString(timeOfDay.trimmed(), "HH:mm");
    if (!time.isValid())
        return {};

    const QTimeZone tz(timezone.trimmed().toUtf8());
    if (!tz.isValid())
        return {};

    // An empty rule means a one-time ("Repeat = Never") net: a single event at
    // startDate + timeOfDay. Returns it only while it is still in the future.
    if (rrule.trimmed().isEmpty()) {
        if (!startDate.isValid())
            return {};
        const QDateTime once = zonedDateTime(startDate, time, tz);
        if (once.isValid() && once.toUTC() > afterUtc.toUTC())
            return once;
        return {};
    }

    const ParsedRule rule = parseRule(rrule);
    if (!rule.isValid())
        return {};

    // Begin the day-by-day scan from the local date of `afterUtc` so we never
    // miss an occurrence later the same day.
    const QDateTime afterLocal = afterUtc.toUTC().toTimeZone(tz);
    const QDate startLocalDate = afterLocal.date();

    for (int offset = 0; offset <= kSearchHorizonDays; ++offset) {
        const QDate candidateDate = startLocalDate.addDays(offset);
        if (!matchesRule(candidateDate, rule, startDate))
            continue;

        // Resolve the wall-clock time within DST transitions (see zonedDateTime).
        const QDateTime candidate = zonedDateTime(candidateDate, time, tz);
        if (!candidate.isValid())
            continue;

        if (candidate.toUTC() > afterUtc.toUTC())
            return candidate;
    }

    return {};
}

QDateTime nextOccurrence(const NetEntry& entry, const QDateTime& afterUtc)
{
    return nextOccurrence(entry.rrule, entry.timeOfDay, entry.timezone,
                          QDate::fromString(entry.startDate, "yyyy-MM-dd"), afterUtc);
}

QString describeRule(const QString& rrule)
{
    if (rrule.trimmed().isEmpty())
        return QStringLiteral("Once");

    const ParsedRule rule = parseRule(rrule);
    if (!rule.isValid())
        return {};

    switch (rule.freq) {
    case ParsedRule::Freq::Daily:
        return rule.interval > 1 ? QString("Every %1 days").arg(rule.interval) : "Daily";
    case ParsedRule::Freq::Weekly: {
        QStringList names;
        for (int dow : rule.weekdays)
            names << weekdayLongName(dow);
        const QString on = names.isEmpty() ? QString() : QString(" on %1").arg(names.join(", "));
        if (rule.interval > 1)
            return QString("Every %1 weeks%2").arg(rule.interval).arg(on);
        return QString("Weekly%1").arg(on);
    }
    case ParsedRule::Freq::Monthly: {
        const QString body = QString("on the %1 %2")
                                 .arg(ordinalName(rule.monthlyOrdinal))
                                 .arg(weekdayLongName(rule.monthlyWeekday));
        if (rule.interval > 1)
            return QString("Every %1 months %2").arg(rule.interval).arg(body);
        return QString("Monthly %1").arg(body);
    }
    case ParsedRule::Freq::Invalid:
        break;
    }
    return {};
}

} // namespace NetRecurrence
} // namespace AetherSDR
