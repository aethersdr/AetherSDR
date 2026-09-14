#pragma once
#include <QByteArray>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>

namespace AetherSDR {
struct OperaRadarImage {
    QImage image;
    QRectF bounds; // EPSG:3857; image top is bounds.bottom() (north).
    QString error;
    static QColor colorForDbz(float dbz);
    static OperaRadarImage decode(const QByteArray& tiff);
    static QPointF projectLaea(double latitude, double longitude);
};
}
