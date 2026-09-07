#pragma once

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

} // namespace AetherSDR
