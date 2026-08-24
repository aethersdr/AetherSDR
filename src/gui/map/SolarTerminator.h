#pragma once

#include <QDateTime>
#include <QtMath>

#include <cmath>

namespace AetherSDR::SolarTerminator {

struct Position {
    double declinationRad{0.0};
    double subsolarLonRad{0.0};
};

inline double normalizeDegrees(double degrees)
{
    double value = std::fmod(degrees + 180.0, 360.0);
    if (value < 0.0) {
        value += 360.0;
    }
    return value - 180.0;
}

// Compact NOAA-style solar position approximation.  It is comfortably more
// precise than the map's raster resolution and remains deterministic in UTC.
inline Position positionAt(const QDateTime& dateTime)
{
    const double julianDay = dateTime.toUTC().toMSecsSinceEpoch()
                           / 86400000.0 + 2440587.5;
    const double days = julianDay - 2451545.0;
    const double meanLongitude = normalizeDegrees(280.460 + 0.9856474 * days);
    const double anomaly = qDegreesToRadians(
        normalizeDegrees(357.528 + 0.9856003 * days));
    const double eclipticLongitude = qDegreesToRadians(
        meanLongitude + 1.915 * std::sin(anomaly)
        + 0.020 * std::sin(2.0 * anomaly));
    const double obliquity = qDegreesToRadians(23.439 - 0.0000004 * days);
    const double rightAscension = std::atan2(
        std::cos(obliquity) * std::sin(eclipticLongitude),
        std::cos(eclipticLongitude));
    const double declination = std::asin(
        std::sin(obliquity) * std::sin(eclipticLongitude));
    const double siderealDegrees = normalizeDegrees(
        280.46061837 + 360.98564736629 * days);
    const double subsolarLon = normalizeDegrees(
        qRadiansToDegrees(rightAscension) - siderealDegrees);
    return { declination, qDegreesToRadians(subsolarLon) };
}

inline double solarElevationDot(double latitudeDeg, double longitudeDeg,
                                const Position& sun)
{
    const double latitude = qDegreesToRadians(latitudeDeg);
    const double hourAngle = qDegreesToRadians(longitudeDeg)
                           - sun.subsolarLonRad;
    return std::sin(latitude) * std::sin(sun.declinationRad)
         + std::cos(latitude) * std::cos(sun.declinationRad)
               * std::cos(hourAngle);
}

inline bool isNight(double latitudeDeg, double longitudeDeg,
                    const Position& sun)
{
    return solarElevationDot(latitudeDeg, longitudeDeg, sun) < 0.0;
}

} // namespace AetherSDR::SolarTerminator
