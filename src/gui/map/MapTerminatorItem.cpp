#include "MapTerminatorItem.h"

#include "SolarTerminator.h"
#include "core/ThemeManager.h"

#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVMapQGItem.h>
#include <QGeoView/QGVMapQGView.h>
#include <QGeoView/QGVProjection.h>

#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QPainter>
#include <QtConcurrent/QtConcurrentRun>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {
constexpr int kRasterWidth = 1440;
constexpr int kRasterHeight = 720;
constexpr int kWorldCopies = 4;
constexpr double kTransitionDot = 0.006;

void disableRedundantDeviceCache(QGVDrawItem* drawItem, QGVMap* geoMap)
{
    for (QGraphicsItem* item : geoMap->geoView()->scene()->items()) {
        if (QGVMapQGItem::geoObjectFromQGItem(item) == drawItem) {
            item->setCacheMode(QGraphicsItem::NoCache);
            return;
        }
    }
}
}

MapTerminatorItem::MapTerminatorItem()
{
    setSelectable(false);
    setZValue(-10);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this] { rebuildImage(); });
}

MapTerminatorItem::~MapTerminatorItem()
{
    if (m_renderCancelled) {
        m_renderCancelled->store(true, std::memory_order_relaxed);
    }
}

void MapTerminatorItem::setDateTime(const QDateTime& dateTime)
{
    m_dateTime = dateTime.toUTC();
    rebuildImage();
}

void MapTerminatorItem::onProjection(QGVMap* geoMap)
{
    QGVDrawItem::onProjection(geoMap);
    disableRedundantDeviceCache(this, geoMap);
    m_worldRect = geoMap->getProjection()->boundaryProjRect();
    rebuildImage();
}

void MapTerminatorItem::rebuildImage()
{
    if (getMap() == nullptr || m_worldRect.isEmpty()) {
        return;
    }
    if (m_renderCancelled) {
        m_renderCancelled->store(true, std::memory_order_relaxed);
    }
    const quint64 generation = ++m_renderGeneration;
    m_renderCancelled = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancelled = m_renderCancelled;
    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this,
            [this, watcher, generation] {
                const QImage image = watcher->result();
                watcher->deleteLater();
                if (generation != m_renderGeneration || image.isNull()) {
                    return;
                }
                m_image = image;
                resetBoundary();
                refresh();
            });
    watcher->setFuture(QtConcurrent::run(
        &MapTerminatorItem::renderImage, m_worldRect, m_dateTime,
        ThemeManager::instance().color("color.background.0"), cancelled));
}

QImage MapTerminatorItem::renderImage(
    QRectF worldRect, QDateTime dateTime, QColor night,
    std::shared_ptr<std::atomic_bool> cancelled)
{
    if (cancelled->load(std::memory_order_relaxed)) {
        return {};
    }
    QImage image(kRasterWidth, kRasterHeight,
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    constexpr int kNightAlpha = 150;
    const SolarTerminator::Position sun =
        SolarTerminator::positionAt(dateTime);

    // MapView always uses QGeoView's EPSG:3857 projection. Deriving latitude
    // and longitude from the immutable world rectangle keeps all million
    // samples off the GUI-owned QGVProjection object and lets this renderer run
    // safely on the global worker pool.
    const double originShift = worldRect.width() / 2.0;
    QVector<double> hourAngleCosines(kRasterWidth);
    for (int x = 0; x < kRasterWidth; ++x) {
        const double projectedX = worldRect.left()
            + (x + 0.5) * worldRect.width() / kRasterWidth;
        const double longitude = projectedX / originShift * 180.0;
        hourAngleCosines[x] = std::cos(
            qDegreesToRadians(longitude) - sun.subsolarLonRad);
    }
    const double sunSin = std::sin(sun.declinationRad);
    const double sunCos = std::cos(sun.declinationRad);
    const int red = night.red();
    const int green = night.green();
    const int blue = night.blue();
    for (int y = 0; y < kRasterHeight; ++y) {
        if (cancelled->load(std::memory_order_relaxed)) {
            return {};
        }
        const double projectedY = worldRect.top()
            + (y + 0.5) * worldRect.height() / kRasterHeight;
        const double preLatitude = -projectedY / originShift * 180.0;
        const double latitude = 2.0 * std::atan(
            std::exp(qDegreesToRadians(preLatitude))) - M_PI / 2.0;
        const double latitudeSin = std::sin(latitude);
        const double latitudeCos = std::cos(latitude);
        QRgb* pixels = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < kRasterWidth; ++x) {
            const double elevationDot = latitudeSin * sunSin
                + latitudeCos * sunCos * hourAngleCosines.at(x);
            const double coverage = std::clamp(
                0.5 - elevationDot / (2.0 * kTransitionDot), 0.0, 1.0);
            if (coverage > 0.0) {
                const int alpha = qRound(kNightAlpha * coverage);
                pixels[x] = qPremultiply(qRgba(red, green, blue, alpha));
            }
        }
    }
    return image;
}

QPainterPath MapTerminatorItem::projShape() const
{
    QPainterPath path;
    for (int copy = -kWorldCopies; copy <= kWorldCopies; ++copy) {
        path.addRect(m_worldRect.translated(copy * m_worldRect.width(), 0.0));
    }
    return path;
}

void MapTerminatorItem::projPaint(QPainter* painter)
{
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (int copy = -kWorldCopies; copy <= kWorldCopies; ++copy) {
        painter->drawImage(
            m_worldRect.translated(copy * m_worldRect.width(), 0.0), m_image);
    }
}

} // namespace AetherSDR
