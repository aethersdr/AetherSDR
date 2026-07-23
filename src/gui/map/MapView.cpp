#include "MapView.h"
#include "MapMarkerItem.h"
#include "MapPathItem.h"

#include <QGeoView/QGVCamera.h>
#include <QGeoView/QGVLayer.h>
#include <QGeoView/QGVLayerOSM.h>
#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVMapQGView.h>
#include <QGeoView/QGVProjection.h>
#include <QGeoView/QGVWidgetScale.h>
#include <QGeoView/QGVWidgetText.h>

#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QShowEvent>
#include <QStandardPaths>
#include <QToolButton>
#include <QToolTip>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <cmath>

namespace AetherSDR {

namespace {
// Initial view when no home position is known yet: whole world.
const QGV::GeoRect kWorldRect{ 70.0, -170.0, -60.0, 170.0 };
// View placed around the home position by resetToHome(): roughly
// continental scale, wide enough that typical HF reception paths fit.
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

    m_map->addWidget(new QGVWidgetScale());

    m_zoomInBtn = makeOverlayButton(QStringLiteral("+"), tr("Zoom in"));
    connect(m_zoomInBtn, &QToolButton::clicked, this, &MapView::zoomIn);
    m_zoomOutBtn = makeOverlayButton(QStringLiteral("−"), tr("Zoom out"));
    connect(m_zoomOutBtn, &QToolButton::clicked, this, &MapView::zoomOut);
    m_homeBtn = makeOverlayButton(QStringLiteral("⌂"), tr("Reset to my location (Home)"));
    connect(m_homeBtn, &QToolButton::clicked, this, &MapView::resetToHome);

    // Sonar pulse on the station marker, every 3 s. The animation timer
    // only runs for the ~1 s ring sweep; idle cost is one tick per period.
    m_pulseAnim = new QVariantAnimation(this);
    m_pulseAnim->setStartValue(0.0);
    m_pulseAnim->setEndValue(1.0);
    m_pulseAnim->setDuration(1000);
    connect(m_pulseAnim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) {
                if (m_homeMarker != nullptr) {
                    m_homeMarker->setPulsePhase(v.toDouble());
                }
            });
    connect(m_pulseAnim, &QVariantAnimation::finished, this, [this] {
        if (m_homeMarker != nullptr) {
            m_homeMarker->setPulsePhase(-1.0);
        }
    });
    m_pulseTimer = new QTimer(this);
    m_pulseTimer->setInterval(3000);
    connect(m_pulseTimer, &QTimer::timeout, this, [this] {
        if (m_homeMarker != nullptr && isVisible()
            && m_pulseAnim->state() != QVariantAnimation::Running) {
            m_pulseAnim->start();
        }
    });
    m_pulseTimer->start();

    setFocusPolicy(Qt::StrongFocus);
    // Keys must reach our keyPressEvent even when the inner QGraphicsView
    // has focus — it would otherwise consume the arrows for scrolling.
    m_map->geoView()->setFocusProxy(this);

    connect(m_map, &QGVMap::itemClicked, this,
            [this](QGVItem* item, QPointF) {
                auto* marker = dynamic_cast<MapMarkerItem*>(item);
                if (marker != nullptr) {
                    emit markerClicked(marker->marker());
                }
            });
    // Double-click anywhere: zoom in anchored on the clicked point.
    connect(m_map, &QGVMap::mapMouseDoubleClicked, this,
            [this](QPointF projPos) {
                m_map->cameraTo(QGVCameraActions(m_map)
                                    .moveTo(projPos)
                                    .scaleBy(kZoomStep),
                                true);
            });

    // Instant hover tooltip. QGeoView's built-in tooltip fires on the OS
    // QEvent::ToolTip (a multi-second wake-up delay), and its mapMouseMove
    // signal doesn't fire for plain hovering (the inner QGraphicsView
    // viewport consumes move events without forwarding). So disable the
    // delayed tooltip and watch the viewport's mouse-move directly.
    m_map->setMouseAction(QGV::MouseAction::Tooltip, false);
    QWidget* vp = m_map->geoView()->viewport();
    vp->setMouseTracking(true);
    vp->installEventFilter(this);

    // Persistent hover card: a frameless child label we show/hide ourselves,
    // so it stays up until the mouse leaves the marker (no QToolTip fade).
    m_hoverCard = new QLabel(this);
    m_hoverCard->setObjectName(QStringLiteral("pskHoverCard"));
    m_hoverCard->setTextFormat(Qt::RichText);
    m_hoverCard->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_hoverCard->setStyleSheet(QStringLiteral(
        "QLabel#pskHoverCard {"
        "  background-color: rgba(28, 28, 30, 235);"
        "  color: #f0f0f0;"
        "  border: 1px solid rgba(255,255,255,40);"
        "  border-radius: 5px; padding: 5px 8px; }"));
    m_hoverCard->hide();
}

bool MapView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_map->geoView()->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(event);
            // Viewport pixels → scene/projection coordinates for the hit test.
            showHoverTooltip(m_map->geoView()->mapToScene(me->pos()));
        } else if (event->type() == QEvent::Leave) {
            m_hoverMarker = nullptr;
            m_hoverCard->hide();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MapView::showHoverTooltip(const QPointF& projPos)
{
    MapMarkerItem* hit = nullptr;
    for (QGVDrawItem* item : m_map->search(projPos)) {
        auto* marker = dynamic_cast<MapMarkerItem*>(item);
        if (marker != nullptr && !marker->marker().tooltip.isEmpty()) {
            hit = marker;
            break;
        }
    }
    if (hit == nullptr) {
        m_hoverMarker = nullptr;
        m_hoverCard->hide();
        return;
    }
    if (hit != m_hoverMarker) {
        m_hoverMarker = hit;
        m_hoverCard->setText(hit->marker().tooltip);
        m_hoverCard->adjustSize();
    }
    // Position near the cursor, clamped to stay fully inside the widget.
    const QPoint cursor = mapFromGlobal(QCursor::pos());
    int x = cursor.x() + 14;
    int y = cursor.y() + 14;
    x = qBound(0, x, width() - m_hoverCard->width());
    y = qBound(0, y, height() - m_hoverCard->height());
    m_hoverCard->move(x, y);
    m_hoverCard->raise();
    m_hoverCard->show();
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

    rebuildPaths();

    if (firstFix && !m_firstShow) {
        resetToHome();
    }
}

void MapView::setHomeSpanDegrees(double spanDegrees)
{
    if (!std::isfinite(spanDegrees) || spanDegrees <= 0.0) {
        return;
    }
    m_homeSpanDeg = qBound(0.002, spanDegrees, 120.0);
}

void MapView::setMarkers(const QVector<Marker>& markers)
{
    clearMarkers();
    m_markerData = markers;
    m_markers.reserve(markers.size());
    for (const Marker& m : markers) {
        auto* item = new MapMarkerItem(m);
        m_markers.append(item);
        m_markerLayer->addItem(item);
    }
    rebuildPaths();
}

void MapView::setPathsVisible(bool visible)
{
    if (m_pathsVisible == visible) {
        return;
    }
    m_pathsVisible = visible;
    rebuildPaths();
}

void MapView::rebuildPaths()
{
    for (MapPathItem* path : std::as_const(m_paths)) {
        m_markerLayer->removeItem(path);
        delete path;
    }
    m_paths.clear();
    if (!m_pathsVisible || !m_hasHome) {
        return;
    }
    m_paths.reserve(m_markerData.size());
    for (const Marker& m : std::as_const(m_markerData)) {
        auto* path = new MapPathItem(m_homeLat, m_homeLon,
                                     m.lat, m.lon, m.color);
        m_paths.append(path);
        m_markerLayer->addItem(path);
    }
}

void MapView::setLegend(const QVector<QPair<QString, QColor>>& entries)
{
    if (entries.isEmpty()) {
        delete m_legend;
        m_legend = nullptr;
        return;
    }
    if (m_legend == nullptr) {
        m_legend = new QLabel(this);
        m_legend->setStyleSheet(QStringLiteral(
            "QLabel { background-color: rgba(40, 40, 40, 190);"
            " color: white; border-radius: 4px; padding: 4px 6px;"
            " font-size: 10px; }"));
        m_legend->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    QString html;
    for (const auto& e : entries) {
        if (!html.isEmpty()) {
            html += QStringLiteral("&nbsp;&nbsp;");
        }
        html += QStringLiteral("<span style=\"color:%1;\">&#9679;</span> %2")
                    .arg(e.second.name(), e.first.toHtmlEscaped());
    }
    m_legend->setText(html);
    m_legend->adjustSize();
    m_legend->show();
    layoutOverlayButtons();
}

void MapView::clearMarkers()
{
    for (MapMarkerItem* item : std::as_const(m_markers)) {
        m_markerLayer->removeItem(item);
        delete item;
    }
    m_markers.clear();
    m_markerData.clear();
    for (MapPathItem* path : std::as_const(m_paths)) {
        m_markerLayer->removeItem(path);
        delete path;
    }
    m_paths.clear();
}

void MapView::resetToHome()
{
    if (!m_hasHome) {
        m_map->cameraTo(QGVCameraActions(m_map).scaleTo(kWorldRect), true);
        return;
    }
    const QGV::GeoRect rect{ m_homeLat + m_homeSpanDeg / 2.0,
                             m_homeLon - m_homeSpanDeg,
                             m_homeLat - m_homeSpanDeg / 2.0,
                             m_homeLon + m_homeSpanDeg };
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

QToolButton* MapView::makeOverlayButton(const QString& text, const QString& tip)
{
    auto* btn = new QToolButton(this);
    btn->setText(text);
    btn->setToolTip(tip);
    btn->setFixedSize(30, 30);
    btn->setCursor(Qt::ArrowCursor);
    btn->setFocusPolicy(Qt::NoFocus);  // keep arrow/+/- keys on the map
    btn->setStyleSheet(QStringLiteral(
        "QToolButton {"
        "  background-color: rgba(40, 40, 40, 200);"
        "  color: white; border: 1px solid rgba(255,255,255,60);"
        "  border-radius: 4px; font-size: 16px; font-weight: bold; }"
        "QToolButton:hover { background-color: rgba(70, 70, 70, 220); }"
        "QToolButton:pressed { background-color: rgba(20, 20, 20, 220); }"));
    btn->raise();
    return btn;
}

void MapView::layoutOverlayButtons()
{
    constexpr int kMargin = 8;
    constexpr int kGap = 6;
    int y = kMargin;
    for (QToolButton* btn : { m_zoomInBtn, m_zoomOutBtn, m_homeBtn }) {
        if (btn == nullptr) {
            continue;
        }
        btn->move(width() - btn->width() - kMargin, y);
        btn->raise();
        y += btn->height() + kGap;
    }
    if (m_legend != nullptr) {
        m_legend->move(kMargin, height() - m_legend->height() - kMargin);
        m_legend->raise();
    }
}

void MapView::clampMinZoomToViewport()
{
    // QGeoView renders a single (non-repeating) world, so zooming out past
    // the point where the world fills the viewport exposes blank tiles on
    // the sides. Pin the minimum scale so the world always covers the view
    // in both axes. Recomputed on every resize.
    auto* view = m_map->geoView();
    const QGVProjection* proj = m_map->getProjection();
    if (view == nullptr || proj == nullptr) {
        return;
    }
    const QRectF world = proj->boundaryProjRect();
    if (world.width() <= 0.0 || world.height() <= 0.0) {
        return;
    }
    const double minScale = qMax(static_cast<double>(width()) / world.width(),
                                 static_cast<double>(height()) / world.height());
    view->setScaleLimits(minScale, view->getMaxScale());
    if (m_map->getCamera().scale() < minScale) {
        m_map->cameraTo(QGVCameraActions(m_map).scaleTo(minScale));
    }
}

void MapView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    clampMinZoomToViewport();
    layoutOverlayButtons();
}

void MapView::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    clampMinZoomToViewport();
    if (m_firstShow) {
        m_firstShow = false;
        resetToHome();
    }
}

} // namespace AetherSDR
