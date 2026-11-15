#pragma once
#include "WeatherRadarViewGeometry.h"
#include <QByteArray>
#include <QDateTime>
#include <QUrl>
#include <QVector>
namespace AetherSDR {
// An observation and every resource referring to it move together at loop wrap.
struct WeatherRadarFrame {
    QDateTime time;
    QDateTime sampleTime;
    QVector<qint64> rasterIds;
    int providers{0};
    QByteArray bytes;
    QUrl url;
    WeatherRadarViewGeometry requestGeometry;
    QString cacheKey;
    QRectF bounds; // canonical EPSG:3857, never QGeoView's reflected coordinates
};
}
