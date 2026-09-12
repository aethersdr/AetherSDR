#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QVector>
#include <cmath>
#include <limits>

namespace AetherSDR {
struct RadarSite {
    QString id;
    QString name;
    QString operatorName;
    double lat{0};
    double lon{0};
    double rangeKm{0};
    QVector<QPointF> ring; // longitude, latitude; geodesic on the map sphere.
    QString description() const
    {
        return QStringLiteral("%1 (%2)\n%3\n%4\nNominal range only; not current radar availability")
            .arg(name, id, operatorName, rangeKm > 0
                ? QStringLiteral("Published maximum range: %1 km").arg(rangeKm)
                : QStringLiteral("Range not published"));
    }
};

inline QVector<QPointF> radarRangeRing(double lat, double lon, double km)
{
    if (!std::isfinite(lat) || !std::isfinite(lon) || !std::isfinite(km)
        || std::abs(lat) > 90 || std::abs(lon) > 180 || km <= 0 || km > 1000) {
        return {};
    }
    constexpr double pi = 3.14159265358979323846;
    const double phi = lat * pi / 180, lambda = lon * pi / 180, d = km / 6371.0088;
    QVector<QPointF> ring;
    ring.reserve(97);
    for (int i = 0; i <= 96; ++i) {
        const double bearing = i * 2 * pi / 96;
        const double p = std::asin(std::sin(phi) * std::cos(d)
            + std::cos(phi) * std::sin(d) * std::cos(bearing));
        const double l = lambda + std::atan2(std::sin(bearing) * std::sin(d) * std::cos(phi),
            std::cos(d) - std::sin(phi) * std::sin(p));
        ring.append(QPointF(std::remainder(l * 180 / pi, 360.0), p * 180 / pi));
    }
    return ring;
}

inline QVector<RadarSite> parseRadarSites(const QByteArray& bytes, bool opera)
{
    if (bytes.size() > 2 * 1024 * 1024) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    const QJsonArray records = opera ? document.array()
        : document.object().value(QStringLiteral("features")).toArray();
    if (records.size() > 5000) {
        return {};
    }
    QVector<RadarSite> result;
    QSet<QString> seen;
    for (const QJsonValue& record : records) {
        const QJsonObject object = record.toObject();
        RadarSite site;
        bool latOk = true, lonOk = true;
        if (opera) {
            if (object.value(QStringLiteral("status")).toString() != QStringLiteral("1")) {
                continue;
            }
            site.id = object.value(QStringLiteral("odimcode")).toString();
            site.name = object.value(QStringLiteral("location")).toString();
            site.operatorName = QStringLiteral("EUMETNET OPERA · %1").arg(object.value(QStringLiteral("country")).toString());
            site.lat = object.value(QStringLiteral("latitude")).toString().toDouble(&latOk);
            site.lon = object.value(QStringLiteral("longitude")).toString().toDouble(&lonOk);
            site.rangeKm = object.value(QStringLiteral("maxrange")).toString().toDouble();
        } else {
            const QJsonObject p = object.value(QStringLiteral("properties")).toObject();
            const QJsonObject geo = object.value(QStringLiteral("geometry")).toObject();
            const QJsonArray coords = geo.value(QStringLiteral("coordinates")).toArray();
            if (geo.value(QStringLiteral("type")).toString() != QStringLiteral("Point")
                || coords.size() < 2 || !coords[0].isDouble() || !coords[1].isDouble()) {
                continue;
            }
            site.id = p.value(QStringLiteral("id")).toString();
            site.name = p.value(QStringLiteral("name")).toString();
            site.operatorName = QStringLiteral("NOAA/NWS radar network · %1").arg(p.value(QStringLiteral("stationType")).toString());
            site.lon = coords[0].toDouble(); site.lat = coords[1].toDouble();
            // ROC publishes 460 km maximum WSR-88D reflectivity range.
            // Other radar types have no inferred range.
            if (p.value(QStringLiteral("stationType")).toString() == QStringLiteral("WSR-88D")) {
                site.rangeKm = 460;
            }
        }
        if (!latOk || !lonOk || !std::isfinite(site.lat) || !std::isfinite(site.lon)
            || std::abs(site.lat) > 90 || std::abs(site.lon) > 180
            || site.id.isEmpty() || site.id.size() > 64 || site.name.size() > 256
            || site.operatorName.size() > 256 || seen.contains(site.id)) {
            continue;
        }
        if (!std::isfinite(site.rangeKm) || site.rangeKm < 0 || site.rangeKm > 1000) {
            site.rangeKm = 0;
        }
        site.ring = radarRangeRing(site.lat, site.lon, site.rangeKm);
        seen.insert(site.id);
        result.append(site);
    }
    return result;
}
} // namespace AetherSDR
