#pragma once

#include <QString>
#include <algorithm>
#include <cmath>

namespace AetherSDR {

// Header-only Maidenhead grid square utilities.
// Supports 4-character and 6-character locators.
class MaidenheadLocator {
public:
    // Decode a Maidenhead locator to the center lat/lon in decimal degrees.
    // Returns false if the locator is invalid or empty.
    static bool toLatLon(const QString& locator, double& lat, double& lon)
    {
        const QString loc = locator.toUpper().trimmed();
        if (loc.size() < 4) return false;

        // Field (A-R)
        int lonDeg = (loc[0].unicode() - 'A') * 20 - 180;
        int latDeg = (loc[1].unicode() - 'A') * 10 - 90;
        if (loc[0] < 'A' || loc[0] > 'R') return false;
        if (loc[1] < 'A' || loc[1] > 'R') return false;

        // Square (0-9)
        if (!loc[2].isDigit() || !loc[3].isDigit()) return false;
        lonDeg += (loc[2].unicode() - '0') * 2;
        latDeg += (loc[3].unicode() - '0');

        if (loc.size() >= 6) {
            // Subsquare (a-x)
            QChar c4 = loc[4].toLower();
            QChar c5 = loc[5].toLower();
            if (c4 < 'a' || c4 > 'x' || c5 < 'a' || c5 > 'x') {
                // Treat as 4-char
                lon = lonDeg + 1.0;
                lat = latDeg + 0.5;
                return true;
            }
            lon = lonDeg + (c4.unicode() - 'a') * (2.0 / 24.0) + (1.0 / 24.0);
            lat = latDeg + (c5.unicode() - 'a') * (1.0 / 24.0) + (0.5 / 24.0);
        } else {
            lon = lonDeg + 1.0;
            lat = latDeg + 0.5;
        }
        return true;
    }

    // Encode a lat/lon (decimal degrees) to a 6-character Maidenhead locator,
    // in the conventional mixed case (field upper, square digits, subsquare
    // lower), e.g. "IM99sc". Returns empty if the inputs are out of range.
    // Inverse of toLatLon(): a value produced by toLatLon() round-trips back to
    // the grid square that contains it.
    static QString toMaidenhead(double lat, double lon)
    {
        if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
            return QString();

        // Shift to 0..360 / 0..180 and nudge the upper edge inward so a point
        // exactly on +180/+90 lands in the last field rather than overflowing.
        double adjLon = std::min(lon + 180.0, 359.999999);
        double adjLat = std::min(lat + 90.0, 179.999999);

        QString loc;
        loc.reserve(6);

        const int lonField = static_cast<int>(adjLon / 20.0);
        const int latField = static_cast<int>(adjLat / 10.0);
        loc.append(QChar::fromLatin1(static_cast<char>('A' + lonField)));
        loc.append(QChar::fromLatin1(static_cast<char>('A' + latField)));
        adjLon -= lonField * 20.0;
        adjLat -= latField * 10.0;

        const int lonSq = static_cast<int>(adjLon / 2.0);
        const int latSq = static_cast<int>(adjLat);
        loc.append(QChar::fromLatin1(static_cast<char>('0' + lonSq)));
        loc.append(QChar::fromLatin1(static_cast<char>('0' + latSq)));
        adjLon -= lonSq * 2.0;
        adjLat -= latSq;

        const int lonSub = static_cast<int>(adjLon / (2.0 / 24.0));
        const int latSub = static_cast<int>(adjLat / (1.0 / 24.0));
        loc.append(QChar::fromLatin1(static_cast<char>('a' + lonSub)));
        loc.append(QChar::fromLatin1(static_cast<char>('a' + latSub)));

        return loc;
    }

    // Haversine distance in km between two lat/lon points (decimal degrees).
    static double distanceKm(double lat1, double lon1, double lat2, double lon2)
    {
        constexpr double R = 6371.0;
        const double dLat = toRad(lat2 - lat1);
        const double dLon = toRad(lon2 - lon1);
        const double a = std::sin(dLat / 2) * std::sin(dLat / 2)
                       + std::cos(toRad(lat1)) * std::cos(toRad(lat2))
                       * std::sin(dLon / 2) * std::sin(dLon / 2);
        const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
        return R * c;
    }

    // Initial bearing in degrees (0-360, 0=North) from point 1 to point 2.
    static double bearingDeg(double lat1, double lon1, double lat2, double lon2)
    {
        const double dLon = toRad(lon2 - lon1);
        const double rlat1 = toRad(lat1);
        const double rlat2 = toRad(lat2);
        const double y = std::sin(dLon) * std::cos(rlat2);
        const double x = std::cos(rlat1) * std::sin(rlat2)
                       - std::sin(rlat1) * std::cos(rlat2) * std::cos(dLon);
        double bearing = std::atan2(y, x) * 180.0 / M_PI;
        return std::fmod(bearing + 360.0, 360.0);
    }

    // Convenience: compute km and bearing between two grid squares.
    // Returns false if either locator is invalid.
    static bool gridDistance(const QString& fromGrid, const QString& toGrid,
                             double& km, double& bearing)
    {
        double lat1, lon1, lat2, lon2;
        if (!toLatLon(fromGrid, lat1, lon1)) return false;
        if (!toLatLon(toGrid,   lat2, lon2)) return false;
        km      = distanceKm(lat1, lon1, lat2, lon2);
        bearing = bearingDeg(lat1, lon1, lat2, lon2);
        return true;
    }

private:
    static double toRad(double deg) { return deg * M_PI / 180.0; }
};

} // namespace AetherSDR
