#pragma once

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>
#include <QTimeZone>

namespace AetherSDR {

struct WeatherRadarLocalFrameTime {
    QString clock;
    QString details;
};

inline WeatherRadarLocalFrameTime weatherRadarLocalFrameTime(
    const QDateTime& observation, const QLocale& locale = QLocale(),
    const QTimeZone& zone = QTimeZone::systemTimeZone())
{
    // Convert the UTC observation, not merely its clock fields: the local
    // calendar date and DST offset can differ from the current machine time.
    const QDateTime local = observation.toTimeZone(zone);
    return {locale.toString(local.time(), QLocale::ShortFormat),
            local.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss t"))};
}

struct WeatherRadarFramePresentation {
    QString text;
    QString tooltip;
};

inline WeatherRadarFramePresentation weatherRadarFramePresentation(
    const QDateTime& observation, bool live,
    const QDateTime& now = QDateTime::currentDateTimeUtc())
{
    if (live) {
        // The live MapServer export supplies pixels, not a scan timestamp.
        // Its five-minute cache key is NOT the age of the displayed weather.
        return {QCoreApplication::translate("PskReporterMapDialog", "Age unknown"),
            QCoreApplication::translate("PskReporterMapDialog",
                "Live radar imagery: observation time is unavailable. Retained imagery "
                "may be old while a refresh is pending or has failed.")};
    }
    const qint64 ageMinutes = qMax<qint64>(0, observation.secsTo(now) / 60);
    const WeatherRadarLocalFrameTime local = weatherRadarLocalFrameTime(observation);
    return {QCoreApplication::translate("PskReporterMapDialog", "%1 (%2m old)")
                .arg(local.clock).arg(ageMinutes),
        QCoreApplication::translate("PskReporterMapDialog",
            "Radar frame: %1 (local time). %2 minutes old. "
            "New observations are checked every minute during playback.")
                .arg(local.details).arg(ageMinutes)};
}

} // namespace AetherSDR
