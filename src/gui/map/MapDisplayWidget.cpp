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
    m_flatView->setHomePosition(lat, lon, label, showMarker);
    if (m_globeView != nullptr) {
        m_globeView->setHomePosition(lat, lon, label, showMarker);
    }
}

void MapDisplayWidget::setHomeSpanDegrees(double spanDegrees)
{
    m_homeSpanDegrees = spanDegrees;
    m_flatView->setHomeSpanDegrees(spanDegrees);
    if (m_globeView != nullptr) {
        m_globeView->setHomeSpanDegrees(spanDegrees);
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
    m_flatView->setMarkers(markers);
    if (m_globeView != nullptr) {
        m_globeView->setMarkers(markers);
    }
}

void MapDisplayWidget::clearMarkers()
{
    m_markers.clear();
    m_flatView->clearMarkers();
    if (m_globeView != nullptr) {
        m_globeView->clearMarkers();
    }
}

void MapDisplayWidget::setPathsVisible(bool visible)
{
    m_pathsVisible = visible;
    m_flatView->setPathsVisible(visible);
    if (m_globeView != nullptr) {
        m_globeView->setPathsVisible(visible);
    }
}

bool MapDisplayWidget::pathsVisible() const
{
    return m_pathsVisible;
}

void MapDisplayWidget::setDayNightTerminatorVisible(bool visible)
{
    m_terminatorVisible = visible;
    m_flatView->setDayNightTerminatorVisible(visible);
    if (m_globeView != nullptr) {
        m_globeView->setDayNightTerminatorVisible(visible);
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
    m_flatView->setLegend(entries);
    if (m_globeView != nullptr) {
        m_globeView->setLegend(entries);
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
    m_globeView->setHomeSpanDegrees(m_homeSpanDegrees);
    if (m_hasHome) {
        m_globeView->setHomePosition(m_homeLat, m_homeLon, m_homeLabel,
                                     m_showHomeMarker);
    }
    m_globeView->setPathsVisible(m_pathsVisible);
    m_globeView->setDayNightTerminatorVisible(m_terminatorVisible);
    m_globeView->setLegend(m_legendEntries);
    m_globeView->setMarkers(m_markers);
}

void MapDisplayWidget::setProjectionMode(ProjectionMode mode)
{
    if (m_projectionMode == mode) {
        return;
    }
    if (mode == ProjectionMode::Globe) {
        ensureGlobeView();
    }
    m_projectionMode = mode;
    m_stack->setCurrentWidget(mode == ProjectionMode::Globe
                                  ? static_cast<QWidget*>(m_globeView)
                                  : static_cast<QWidget*>(m_flatView));
    emit projectionModeChanged(mode);
}

bool MapDisplayWidget::globeAvailable() const
{
    return true;
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
