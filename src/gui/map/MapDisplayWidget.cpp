#include "MapDisplayWidget.h"
#include "GlobeMapView.h"

#include <QStackedLayout>

namespace AetherSDR {

MapDisplayWidget::MapDisplayWidget(QWidget* parent)
    : QWidget(parent)
    , m_stack(new QStackedLayout(this))
    , m_flatView(new MapView(this))
    , m_globeView(new GlobeMapView(this))
{
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->addWidget(m_flatView);
    m_stack->addWidget(m_globeView);
    connect(m_flatView, &MapView::markerClicked,
            this, &MapDisplayWidget::markerClicked);
    connect(m_globeView, &GlobeMapView::markerClicked,
            this, &MapDisplayWidget::markerClicked);
}

void MapDisplayWidget::setHomePosition(double lat, double lon,
                                       const QString& label, bool showMarker)
{
    m_flatView->setHomePosition(lat, lon, label, showMarker);
    m_globeView->setHomePosition(lat, lon, label, showMarker);
}

void MapDisplayWidget::setHomeSpanDegrees(double spanDegrees)
{
    m_flatView->setHomeSpanDegrees(spanDegrees);
    m_globeView->setHomeSpanDegrees(spanDegrees);
}

bool MapDisplayWidget::hasHomePosition() const
{
    return m_flatView->hasHomePosition();
}

double MapDisplayWidget::homeLat() const
{
    return m_flatView->homeLat();
}

double MapDisplayWidget::homeLon() const
{
    return m_flatView->homeLon();
}

void MapDisplayWidget::setMarkers(const QVector<Marker>& markers)
{
    m_flatView->setMarkers(markers);
    m_globeView->setMarkers(markers);
}

void MapDisplayWidget::clearMarkers()
{
    m_flatView->clearMarkers();
    m_globeView->clearMarkers();
}

void MapDisplayWidget::setPathsVisible(bool visible)
{
    m_flatView->setPathsVisible(visible);
    m_globeView->setPathsVisible(visible);
}

bool MapDisplayWidget::pathsVisible() const
{
    return m_flatView->pathsVisible();
}

void MapDisplayWidget::setDayNightTerminatorVisible(bool visible)
{
    m_flatView->setDayNightTerminatorVisible(visible);
    m_globeView->setDayNightTerminatorVisible(visible);
}

bool MapDisplayWidget::dayNightTerminatorVisible() const
{
    return m_flatView->dayNightTerminatorVisible();
}

void MapDisplayWidget::setLegend(
    const QVector<QPair<QString, QColor>>& entries)
{
    m_flatView->setLegend(entries);
    m_globeView->setLegend(entries);
}

void MapDisplayWidget::setProjectionMode(ProjectionMode mode)
{
    if (m_projectionMode == mode) {
        return;
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
