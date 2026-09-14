#include "MapDisplayWidget.h"
#include "WeatherRadarController.h"
#include "WeatherRadarLegend.h"
#include "OperaRadarNetwork.h"
#include "CityLightsSource.h"
#include "GlobeMapView.h"
#include "core/ThemeManager.h"

#include <QAccessible>
#include <QEvent>
#include <QLabel>
#include <QStackedLayout>
#include <QTimer>

#include <algorithm>

namespace AetherSDR {

namespace {
// On macOS + GPU spectrum, attaching a QOpenGLWidget viewport can force a
// backing-store repaint that re-enters a Metal-backed QRhiWidget (the
// panadapter or WAVE scope) and blows up in
// QMetalGraphicsPipeline::makeActiveForCurrentRenderPassEncoder. Keep the
// flat PSK map on QGraphicsView's raster viewport in that configuration.
// Discovered while opening Tools → PSK Reporter on a GPU-spectrum build
// (PR #5595). Not a performance claim — compatibility only.
MapView::ViewportMode flatMapViewportMode()
{
#if defined(Q_OS_MAC) && defined(AETHER_GPU_SPECTRUM)
    return MapView::ViewportMode::Raster;
#else
    return MapView::ViewportMode::OpenGlIfAvailable;
#endif
}

}

MapDisplayWidget::MapDisplayWidget(QWidget* parent)
    : QWidget(parent)
    , m_stack(new QStackedLayout(this))
    , m_flatView(new MapView(this, flatMapViewportMode()))
{
    installOperaRadarNetwork();
    m_cityLightsSource = new CityLightsSource(this);
    connect(m_cityLightsSource, &CityLightsSource::imageChanged,
            this, &MapDisplayWidget::presentCityLights);
    connect(m_cityLightsSource, &CityLightsSource::statusChanged,
            this, &MapDisplayWidget::cityLightsStatusChanged);
    connect(m_flatView, &MapView::imageOverlayViewChanged,
            this, &MapDisplayWidget::refreshCityLightsView);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->addWidget(m_flatView);
    connect(m_flatView, &MapView::markerClicked,
            this, &MapDisplayWidget::markerClicked);
    m_weatherRadar = new WeatherRadarController(m_flatView, this);
    m_radarLegend = new WeatherRadarLegend(this);
    m_radarLegend->installEventFilter(this);
    connect(m_weatherRadar, &WeatherRadarController::displayedProvidersChanged,
        m_radarLegend, &WeatherRadarLegend::setProviders);
    connect(m_weatherRadar, &WeatherRadarController::radarCoverageStatusChanged, this, &MapDisplayWidget::radarCoverageStatusChanged);
    connect(m_weatherRadar, &WeatherRadarController::radarProviderStatusChanged, this, &MapDisplayWidget::radarProviderStatusChanged);
    connect(m_weatherRadar, &WeatherRadarController::weatherRadarAnimationStateChanged, this, &MapDisplayWidget::weatherRadarAnimationStateChanged);
    connect(m_weatherRadar, &WeatherRadarController::weatherRadarTimelineLoadingChanged, this, &MapDisplayWidget::weatherRadarTimelineLoadingChanged);
    connect(m_weatherRadar, &WeatherRadarController::weatherRadarFrameChanged, this, &MapDisplayWidget::weatherRadarFrameChanged);
    connect(m_weatherRadar, &WeatherRadarController::weatherRadarAnimationError, this, &MapDisplayWidget::weatherRadarAnimationError);
    connect(m_weatherRadar, &WeatherRadarController::loadingStatusChanged, this,
        [this](const QString& text, const QString& announcement) {
            m_weatherRadarLoadingText = text;
            m_weatherRadarLoadingAnnouncement = announcement;
            updateOverlayLoadingStatus();
        });

    m_weatherRadarLoadingLabel = new QLabel(this);
    m_weatherRadarLoadingLabel->setObjectName(QStringLiteral("pskReporterWeatherRadarLoading"));
    m_weatherRadarLoadingLabel->setAccessibleName(tr("Map overlay loading status"));
    m_weatherRadarLoadingLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_weatherRadarLoadingLabel->setFocusPolicy(Qt::NoFocus);
    m_weatherRadarLoadingLabel->setAlignment(Qt::AlignCenter);
    m_weatherRadarLoadingLabel->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(m_weatherRadarLoadingLabel,
        "QLabel { background: {{color.background.2}}; color: {{color.text.primary}};"
        " border: 1px solid {{color.border.subtle}}; border-radius: 6px; padding: 5px 10px; }");
    m_weatherRadarLoadingLabel->hide();
    m_cityLightsLoadingTimer = new QTimer(this);
    m_cityLightsLoadingTimer->setSingleShot(true);
    m_cityLightsLoadingTimer->setInterval(350);
    connect(m_cityLightsLoadingTimer, &QTimer::timeout,
            this, &MapDisplayWidget::updateOverlayLoadingStatus);
    connect(this, &MapDisplayWidget::cityLightsStatusChanged, this, [this](const QString& status) {
        m_cityLightsLoadingText = status;
        if (status.isEmpty()) {
            m_cityLightsLoadingTimer->stop();
        } else {
            // Avoid flashing a notice for quick cache hits.
            m_cityLightsLoadingTimer->start();
        }
        updateOverlayLoadingStatus();
    });

}

void MapDisplayWidget::setHomePosition(double lat, double lon,
                                       const QString& label, bool showMarker)
{
    m_hasHome = true;
    m_homeLat = lat;
    m_homeLon = lon;
    m_homeLabel = label;
    m_showHomeMarker = showMarker;
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setHomePosition(lat, lon, label, showMarker);
        m_flatViewDirty = true;
    } else {
        m_flatView->setHomePosition(lat, lon, label, showMarker);
        m_globeViewDirty = m_globeView != nullptr;
    }
}

void MapDisplayWidget::setHomeSpanDegrees(double spanDegrees)
{
    m_homeSpanDegrees = spanDegrees;
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setHomeSpanDegrees(spanDegrees);
        m_flatViewDirty = true;
    } else {
        m_flatView->setHomeSpanDegrees(spanDegrees);
        m_globeViewDirty = m_globeView != nullptr;
    }
}

bool MapDisplayWidget::hasHomePosition() const
{
    return m_hasHome;
}

double MapDisplayWidget::homeLat() const
{
    return m_homeLat;
}

double MapDisplayWidget::homeLon() const
{
    return m_homeLon;
}

void MapDisplayWidget::setMarkers(const QVector<Marker>& markers)
{
    m_markers = markers;
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setMarkers(markers);
        m_flatViewDirty = true;
    } else {
        m_flatView->setMarkers(markers);
        m_globeViewDirty = m_globeView != nullptr;
    }
}

void MapDisplayWidget::clearMarkers()
{
    m_markers.clear();
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->clearMarkers();
        m_flatViewDirty = true;
    } else {
        m_flatView->clearMarkers();
        m_globeViewDirty = m_globeView != nullptr;
    }
}

void MapDisplayWidget::setPathsVisible(bool visible)
{
    m_pathsVisible = visible;
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setPathsVisible(visible);
        m_flatViewDirty = true;
    } else {
        m_flatView->setPathsVisible(visible);
        m_globeViewDirty = m_globeView != nullptr;
    }
}

bool MapDisplayWidget::pathsVisible() const
{
    return m_pathsVisible;
}

void MapDisplayWidget::setDayNightTerminatorVisible(bool visible)
{
    m_terminatorVisible = visible;
    m_cityLightsSource->setNightOnly(visible && m_projectionMode == ProjectionMode::Flat);
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setDayNightTerminatorVisible(visible);
        m_flatViewDirty = true;
    } else {
        m_flatView->setDayNightTerminatorVisible(visible);
        m_globeViewDirty = m_globeView != nullptr;
    }
}

bool MapDisplayWidget::dayNightTerminatorVisible() const
{
    return m_terminatorVisible;
}

void MapDisplayWidget::setDetailedAttributionVisible(bool visible)
{
    m_detailedAttributionVisible = visible;
    m_flatView->setDetailedAttributionVisible(visible);
    if (m_globeView != nullptr) {
        m_globeView->setDetailedAttributionVisible(visible);
    }
}

void MapDisplayWidget::setCityLightsVisible(bool visible)
{
    m_cityLightsVisible = visible;
    updateOverlayLoadingStatus();
    m_cityLightsSource->setNightOnly(m_terminatorVisible && m_projectionMode == ProjectionMode::Flat);
    m_cityLightsSource->setEnabled(visible && isVisible());
    presentCityLights();
    refreshCityLightsView();
}

void MapDisplayWidget::setCityLightsWarmth(int percent)
{
    m_cityLightsWarmth = std::clamp(percent, 0, 100);
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setCityLightsWarmth(m_cityLightsWarmth);
    } else {
        m_cityLightsSource->setWarmth(m_cityLightsWarmth);
    }
}

void MapDisplayWidget::setCityLightsFaintLights(int percent)
{
    m_cityLightsFaintLights = std::clamp(percent, 0, 100);
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setCityLightsFaintLights(m_cityLightsFaintLights);
    } else {
        m_cityLightsSource->setFaintLights(m_cityLightsFaintLights);
    }
}

void MapDisplayWidget::setBasemapDarkEnabled(bool enabled)
{
    m_basemapDarkEnabled = enabled;
    m_flatView->setBasemapDarkEnabled(enabled);
    if (m_globeView != nullptr) {
        m_globeView->setBasemapDarkEnabled(enabled);
    }
}

void MapDisplayWidget::setBasemapBrightness(int percent)
{
    m_basemapBrightness = std::clamp(percent, 20, 100);
    m_flatView->setBasemapBrightness(m_basemapBrightness);
    if (m_globeView != nullptr) {
        m_globeView->setBasemapBrightness(m_basemapBrightness);
    }
}

void MapDisplayWidget::setCityLightsBrightness(int percent)
{
    m_cityLightsBrightness = std::clamp(percent, 0, 100);
    presentCityLights();
}

void MapDisplayWidget::presentCityLights()
{
    // Keep pixels with the bounds of the completed render, including while a
    // different viewport image is downloading or its twilight mask is pending.
    m_flatView->setCityLightsVisible(m_cityLightsVisible);
    m_flatView->setCityLightsBrightness(m_cityLightsBrightness);
    // The CPU render only carries flat-map parameters while the flat view is
    // current: in globe mode the source renders the plain original for the
    // GPU. Never hand that, or a render still catching up after a projection
    // switch, to the flat item; imageChanged delivers the correct one.
    if (m_projectionMode == ProjectionMode::Flat && !m_cityLightsSource->renderPending()) {
        m_flatView->setCityLightsImage(m_cityLightsSource->image(), m_cityLightsSource->bounds());
    }
    if (m_globeView != nullptr) {
        m_globeView->setCityLightsVisible(m_cityLightsVisible);
        m_globeView->setCityLightsBrightness(m_cityLightsBrightness);
        m_globeView->setCityLightsFaintLights(m_cityLightsFaintLights);
        m_globeView->setCityLightsWarmth(m_cityLightsWarmth);
        m_globeView->setCityLightsImage(m_cityLightsSource->originalImage(), m_cityLightsSource->originalBounds());
    }
}

void MapDisplayWidget::refreshCityLightsView()
{
    if (m_cityLightsVisible && isVisible()) {
        if (m_projectionMode == ProjectionMode::Flat) {
            m_cityLightsSource->setView({WeatherRadarSource::conventionalBoundsFromQgv(
                m_flatView->weatherRadarPlaybackBounds()), m_flatView->weatherRadarPlaybackSize()});
        } else {
            const QRectF bounds = m_globeView->weatherRadarPlaybackBounds();
            m_cityLightsSource->setView({bounds, m_globeView->weatherRadarPlaybackSize(bounds)});
        }
    }
}

void MapDisplayWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_cityLightsSource->setEnabled(m_cityLightsVisible);
    refreshCityLightsView();
}

void MapDisplayWidget::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_cityLightsSource->setEnabled(false);
}

void MapDisplayWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_radarLegend) { m_radarLegend->setMaximumWidth(std::max(1, width() - 70)); }
    positionRadarLegend();
    updateOverlayLoadingStatus();
}

bool MapDisplayWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_radarLegend && event->type() == QEvent::Resize) {
        positionRadarLegend();
    }
    return QWidget::eventFilter(watched, event);
}

void MapDisplayWidget::positionRadarLegend()
{
    if (m_radarLegend) {
        m_radarLegend->move(12, m_radarLegendAtTop ? 12
            : std::max(12, height() - m_radarLegend->height() - 12));
    }
}

void MapDisplayWidget::setRadarLegendVisible(bool visible)
{
    m_radarLegend->setLegendVisible(visible);
    positionRadarLegend();
}

void MapDisplayWidget::setRadarLegendAtTop(bool atTop)
{
    m_radarLegendAtTop = atTop;
    positionRadarLegend();
}

void MapDisplayWidget::updateOverlayLoadingStatus()
{
    QStringList rows;
    QStringList announcements;
    if (m_weatherRadar->weatherRadarVisible() && !m_weatherRadarLoadingText.isEmpty()) {
        rows.append(m_weatherRadarLoadingText);
        announcements.append(m_weatherRadarLoadingAnnouncement);
    }
    if (m_cityLightsVisible && !m_cityLightsLoadingTimer->isActive()
        && !m_cityLightsLoadingText.isEmpty()) {
        rows.append(m_cityLightsLoadingText);
        announcements.append(m_cityLightsLoadingText);
    }
    if (rows.isEmpty()) {
        m_weatherRadarLoadingLabel->hide();
        if (!m_overlayLoadingAnnouncement.isEmpty()) {
            m_overlayLoadingAnnouncement.clear();
            m_weatherRadarLoadingLabel->setAccessibleDescription(QString());
            QAccessibleEvent event(m_weatherRadarLoadingLabel, QAccessible::DescriptionChanged);
            QAccessible::updateAccessibility(&event);
        }
        return;
    }
    m_weatherRadarLoadingLabel->setText(rows.join(QLatin1Char('\n')));
    m_weatherRadarLoadingLabel->setMaximumWidth(std::max(1, width() - 32));
    m_weatherRadarLoadingLabel->adjustSize();
    m_weatherRadarLoadingLabel->move(
        (width() - m_weatherRadarLoadingLabel->width()) / 2,
        std::max(0, height() - m_weatherRadarLoadingLabel->height() - 36));
    m_weatherRadarLoadingLabel->show();
    m_weatherRadarLoadingLabel->raise();
    // Announce state transitions only, not every completed image/tile.
    const QString message = announcements.join(QLatin1Char('\n'));
    if (m_overlayLoadingAnnouncement != message) {
        m_overlayLoadingAnnouncement = message;
        m_weatherRadarLoadingLabel->setAccessibleDescription(message);
        QAccessibleEvent event(m_weatherRadarLoadingLabel, QAccessible::DescriptionChanged);
        QAccessible::updateAccessibility(&event);
    }
}

void MapDisplayWidget::setLegend(
    const QVector<QPair<QString, QColor>>& entries)
{
    m_legendEntries = entries;
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setLegend(entries);
        m_flatViewDirty = true;
    } else {
        m_flatView->setLegend(entries);
        m_globeViewDirty = m_globeView != nullptr;
    }
}

void MapDisplayWidget::ensureGlobeView()
{
    if (m_globeView != nullptr) {
        return;
    }
    m_globeView = new GlobeMapView(this);
    m_globeView->setDetailedAttributionVisible(m_detailedAttributionVisible);
    m_stack->addWidget(m_globeView);
    connect(m_globeView, &GlobeMapView::markerClicked,
            this, &MapDisplayWidget::markerClicked);
    connect(m_globeView, &GlobeMapView::rendererUnavailable,
            this, &MapDisplayWidget::handleGlobeUnavailable);
    connect(m_globeView, &GlobeMapView::imageOverlayViewChanged,
            this, &MapDisplayWidget::refreshCityLightsView);
    m_weatherRadar->attachGlobe(m_globeView);
    m_globeView->setBasemapDarkEnabled(m_basemapDarkEnabled);
    m_globeView->setBasemapBrightness(m_basemapBrightness);
    m_globeView->setHomeSpanDegrees(m_homeSpanDegrees);
    if (m_hasHome) {
        m_globeView->setHomePosition(m_homeLat, m_homeLon, m_homeLabel,
                                     m_showHomeMarker);
    }
    m_globeView->setPathsVisible(m_pathsVisible);
    m_globeView->setDayNightTerminatorVisible(m_terminatorVisible);
    m_weatherRadar->synchronizeRenderer(true);
    m_globeView->setLegend(m_legendEntries);
    m_globeView->setMarkers(m_markers);
    m_globeViewDirty = false;
}

void MapDisplayWidget::synchronizeFlatView()
{
    if (!m_flatViewDirty) {
        return;
    }
    // Synchronization happens while this renderer is hidden. Disable paths
    // first so applying home and marker changes cannot rebuild an obsolete
    // path batch before the final visibility state is restored.
    m_flatView->setPathsVisible(false);
    m_flatView->setHomeSpanDegrees(m_homeSpanDegrees);
    if (m_hasHome) {
        m_flatView->setHomePosition(m_homeLat, m_homeLon, m_homeLabel,
                                    m_showHomeMarker);
    }
    m_flatView->setDayNightTerminatorVisible(m_terminatorVisible);
    m_weatherRadar->synchronizeRenderer(false);
    m_flatView->setLegend(m_legendEntries);
    m_flatView->setMarkers(m_markers);
    m_flatView->setPathsVisible(m_pathsVisible);
    m_flatViewDirty = false;
}

void MapDisplayWidget::synchronizeGlobeView()
{
    ensureGlobeView();
    if (!m_globeViewDirty) {
        return;
    }
    m_globeView->setBasemapDarkEnabled(m_basemapDarkEnabled);
    m_globeView->setBasemapBrightness(m_basemapBrightness);
    m_globeView->setHomeSpanDegrees(m_homeSpanDegrees);
    if (m_hasHome) {
        m_globeView->setHomePosition(m_homeLat, m_homeLon, m_homeLabel,
                                     m_showHomeMarker);
    }
    m_globeView->setPathsVisible(m_pathsVisible);
    m_globeView->setDayNightTerminatorVisible(m_terminatorVisible);
    m_weatherRadar->synchronizeRenderer(true);
    m_globeView->setLegend(m_legendEntries);
    m_globeView->setMarkers(m_markers);
    m_globeViewDirty = false;
}

void MapDisplayWidget::setProjectionMode(ProjectionMode mode)
{
    if (mode == ProjectionMode::Globe && !m_globeAvailable) {
        return;
    }
    if (m_projectionMode == mode) {
        return;
    }
    if (mode == ProjectionMode::Globe) {
        synchronizeGlobeView();
    } else {
        synchronizeFlatView();
    }
    m_projectionMode = mode;
    m_stack->setCurrentWidget(mode == ProjectionMode::Globe
                                  ? static_cast<QWidget*>(m_globeView)
                                  : static_cast<QWidget*>(m_flatView));
    m_weatherRadar->setGlobeActive(mode == ProjectionMode::Globe);
    m_radarLegend->raise();
    // Flat view uses CPU processing; the globe shades the original on the GPU.
    m_cityLightsSource->setNightOnly(m_terminatorVisible && mode == ProjectionMode::Flat);
    m_cityLightsSource->setFaintLights(mode == ProjectionMode::Flat ? m_cityLightsFaintLights : 0);
    m_cityLightsSource->setWarmth(mode == ProjectionMode::Flat ? m_cityLightsWarmth : 0);
    presentCityLights();
    refreshCityLightsView();
    emit projectionModeChanged(mode);
}

bool MapDisplayWidget::globeAvailable() const
{
    return m_globeAvailable;
}

void MapDisplayWidget::handleGlobeUnavailable(const QString& reason)
{
    if (!m_globeAvailable) {
        return;
    }
    m_globeAvailable = false;
    m_globeUnavailableReason = reason;
    if (m_projectionMode == ProjectionMode::Globe) {
        setProjectionMode(ProjectionMode::Flat);
    }
    emit globeAvailabilityChanged(false, reason);
}

void MapDisplayWidget::resetToHome()
{
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->resetToHome();
    } else {
        m_flatView->resetToHome();
    }
}

void MapDisplayWidget::zoomIn()
{
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->zoomIn();
    } else {
        m_flatView->zoomIn();
    }
}

void MapDisplayWidget::zoomOut()
{
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->zoomOut();
    } else {
        m_flatView->zoomOut();
    }
}

MapDisplayWidget::~MapDisplayWidget() { delete m_weatherRadar; }

void MapDisplayWidget::setRadarCoverageVisible(bool visible)
{
    m_weatherRadar->setRadarCoverageVisible(visible);
}

void MapDisplayWidget::setWeatherRadarVisible(bool visible)
{
    m_weatherRadar->setWeatherRadarVisible(visible);
}

void MapDisplayWidget::setWeatherRadarProvider(WeatherRadarSource::Provider provider)
{
    m_weatherRadar->setWeatherRadarProvider(provider);
}

void MapDisplayWidget::setWeatherRadarRegions(int enabledProviders)
{
    m_weatherRadar->setWeatherRadarRegions(enabledProviders);
}

void MapDisplayWidget::switchWeatherRadarSource(const WeatherRadarSource& source)
{
    m_weatherRadar->switchWeatherRadarSource(source);
}

void MapDisplayWidget::startWeatherRadarAnimation(int historyHours)
{
    m_weatherRadar->startWeatherRadarAnimation(historyHours);
}

void MapDisplayWidget::stopWeatherRadarAnimation()
{
    m_weatherRadar->stopWeatherRadarAnimation();
}

void MapDisplayWidget::setWeatherRadarPlaybackSpeed(int speedPercent)
{
    m_weatherRadar->setWeatherRadarPlaybackSpeed(speedPercent);
}

bool MapDisplayWidget::weatherRadarVisible() const { return m_weatherRadar->weatherRadarVisible(); }
bool MapDisplayWidget::weatherRadarAnimating() const { return m_weatherRadar->weatherRadarAnimating(); }

} // namespace AetherSDR
