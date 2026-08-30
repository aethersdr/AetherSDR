#include "MapDisplayWidget.h"
#include "GlobeMapView.h"

#include <QStackedLayout>

namespace AetherSDR {

MapDisplayWidget::MapDisplayWidget(QWidget* parent)
    : QWidget(parent)
    , m_stack(new QStackedLayout(this))
    , m_flatView(new MapView(this))
{
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->addWidget(m_flatView);
    connect(m_flatView, &MapView::markerClicked,
            this, &MapDisplayWidget::markerClicked);
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
    m_stack->addWidget(m_globeView);
    connect(m_globeView, &GlobeMapView::markerClicked,
            this, &MapDisplayWidget::markerClicked);
    connect(m_globeView, &GlobeMapView::rendererUnavailable,
            this, &MapDisplayWidget::handleGlobeUnavailable);
    m_globeView->setHomeSpanDegrees(m_homeSpanDegrees);
    if (m_hasHome) {
        m_globeView->setHomePosition(m_homeLat, m_homeLon, m_homeLabel,
                                     m_showHomeMarker);
    }
    m_globeView->setPathsVisible(m_pathsVisible);
    m_globeView->setDayNightTerminatorVisible(m_terminatorVisible);
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
    m_globeView->setHomeSpanDegrees(m_homeSpanDegrees);
    if (m_hasHome) {
        m_globeView->setHomePosition(m_homeLat, m_homeLon, m_homeLabel,
                                     m_showHomeMarker);
    }
    m_globeView->setPathsVisible(m_pathsVisible);
    m_globeView->setDayNightTerminatorVisible(m_terminatorVisible);
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

} // namespace AetherSDR
