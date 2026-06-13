#include "MapView.h"
#include "MapMarkerItem.h"

#include <QGeoView/QGVCamera.h>
#include <QGeoView/QGVLayer.h>
#include <QGeoView/QGVLayerOSM.h>
#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVMapQGView.h>
#include <QGeoView/QGVWidgetText.h>

#include <QCoreApplication>
#include <QDir>
#include <QKeyEvent>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QShowEvent>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {
// Initial view when no home position is known yet: whole world.
const QGV::GeoRect kWorldRect{ 70.0, -170.0, -60.0, 170.0 };
// View placed around the home position by resetToHome(): roughly
// continental scale, wide enough that typical HF reception paths fit.
constexpr double kHomeSpanDeg = 30.0;
constexpr double kPanFraction = 0.25;   // arrow-key pan, fraction of viewport
constexpr double kZoomStep = 2.0;       // +/- key zoom factor
constexpr qint64 kTileCacheBytes = 256LL * 1024 * 1024;
} // namespace

void MapView::ensureTileNetworkManager()
{
    if (QGV::getNetworkManager() != nullptr) {
        return;
    }
    // Process-wide manager shared by every MapView. The disk cache honors
    // the HTTP cache headers OSM serves — required by the OSM tile usage
    // policy — and the User-Agent uniquely identifies AetherSDR (library
    // defaults and browser impersonation are documented blocking causes).
    auto* nam = new QNetworkAccessManager(QCoreApplication::instance());
    auto* cache = new QNetworkDiskCache(nam);
    cache->setCacheDirectory(
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QDir::separator() + QStringLiteral("osm-tiles"));
    cache->setMaximumCacheSize(kTileCacheBytes);
    nam->setCache(cache);
    QGV::setNetworkManager(nam);
    QGV::setTileUserAgent(
        QStringLiteral("AetherSDR/%1 (https://github.com/aethersdr/AetherSDR)")
            .arg(QCoreApplication::applicationVersion())
            .toUtf8());
}

MapView::MapView(QWidget* parent)
    : QWidget(parent)
{
    ensureTileNetworkManager();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_map = new QGVMap(this);
    layout->addWidget(m_map);

    m_map->addItem(new QGVLayerOSM());

    m_markerLayer = new QGVLayer();
    m_markerLayer->setName(QStringLiteral("Markers"));
    m_map->addItem(m_markerLayer);

    // Mandatory attribution per the OSM tile usage policy.
    auto* attribution = new QGVWidgetText();
    attribution->setText(QStringLiteral("© OpenStreetMap contributors"));
    m_map->addWidget(attribution);

    setFocusPolicy(Qt::StrongFocus);
    // Keys must reach our keyPressEvent even when the inner QGraphicsView
    // has focus — it would otherwise consume the arrows for scrolling.
    m_map->geoView()->setFocusProxy(this);
}

void MapView::setHomePosition(double lat, double lon, const QString& label,
                              bool showMarker)
{
    const bool firstFix = !m_hasHome;
    m_homeLat = lat;
    m_homeLon = lon;
    m_homeLabel = label;
    m_hasHome = true;

    if (showMarker) {
        Marker home;
        home.lat = lat;
        home.lon = lon;
        home.label = label;
        home.tooltip = label.isEmpty() ? QStringLiteral("Station location")
                                       : label;
        home.color = QColor(0, 122, 255);
        home.isHome = true;
        if (m_homeMarker == nullptr) {
            m_homeMarker = new MapMarkerItem(home);
            m_homeMarker->setZValue(10);
            m_markerLayer->addItem(m_homeMarker);
        } else {
            m_homeMarker->setMarker(home);
        }
    }

    if (firstFix && !m_firstShow) {
        resetToHome();
    }
}

void MapView::setMarkers(const QVector<Marker>& markers)
{
    clearMarkers();
    m_markers.reserve(markers.size());
    for (const Marker& m : markers) {
        auto* item = new MapMarkerItem(m);
        m_markers.append(item);
        m_markerLayer->addItem(item);
    }
}

void MapView::clearMarkers()
{
    for (MapMarkerItem* item : std::as_const(m_markers)) {
        m_markerLayer->removeItem(item);
        delete item;
    }
    m_markers.clear();
}

void MapView::resetToHome()
{
    if (!m_hasHome) {
        m_map->cameraTo(QGVCameraActions(m_map).scaleTo(kWorldRect), true);
        return;
    }
    const QGV::GeoRect rect{ m_homeLat + kHomeSpanDeg / 2.0,
                             m_homeLon - kHomeSpanDeg,
                             m_homeLat - kHomeSpanDeg / 2.0,
                             m_homeLon + kHomeSpanDeg };
    m_map->cameraTo(QGVCameraActions(m_map).scaleTo(rect), true);
}

void MapView::zoomIn()
{
    m_map->cameraTo(QGVCameraActions(m_map).scaleBy(kZoomStep), true);
}

void MapView::zoomOut()
{
    m_map->cameraTo(QGVCameraActions(m_map).scaleBy(1.0 / kZoomStep), true);
}

void MapView::pan(double dxFraction, double dyFraction)
{
    const QRectF projRect = m_map->getCamera().projRect();
    const QPointF delta(projRect.width() * dxFraction,
                        projRect.height() * dyFraction);
    m_map->cameraTo(
        QGVCameraActions(m_map).moveTo(projRect.center() + delta), true);
}

void MapView::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Left:
        pan(-kPanFraction, 0.0);
        break;
    case Qt::Key_Right:
        pan(kPanFraction, 0.0);
        break;
    case Qt::Key_Up:
        pan(0.0, -kPanFraction);
        break;
    case Qt::Key_Down:
        pan(0.0, kPanFraction);
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomIn();
        break;
    case Qt::Key_Minus:
        zoomOut();
        break;
    case Qt::Key_Home:
        resetToHome();
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void MapView::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_firstShow) {
        m_firstShow = false;
        resetToHome();
    }
}

} // namespace AetherSDR
