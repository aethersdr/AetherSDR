#include "MapDisplayWidget.h"

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
    m_flatView->setHomePosition(lat, lon, label, showMarker);
}

void MapDisplayWidget::setHomeSpanDegrees(double spanDegrees)
{
    m_flatView->setHomeSpanDegrees(spanDegrees);
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
}

void MapDisplayWidget::clearMarkers()
{
    m_flatView->clearMarkers();
}

void MapDisplayWidget::setPathsVisible(bool visible)
{
    m_flatView->setPathsVisible(visible);
}

bool MapDisplayWidget::pathsVisible() const
{
    return m_flatView->pathsVisible();
}

void MapDisplayWidget::setDayNightTerminatorVisible(bool visible)
{
    m_flatView->setDayNightTerminatorVisible(visible);
}

bool MapDisplayWidget::dayNightTerminatorVisible() const
{
    return m_flatView->dayNightTerminatorVisible();
}

void MapDisplayWidget::setLegend(
    const QVector<QPair<QString, QColor>>& entries)
{
    m_flatView->setLegend(entries);
}

void MapDisplayWidget::setProjectionMode(ProjectionMode mode)
{
    // Globe construction lands in the next staged commit. Until then this
    // facade deliberately preserves the shipping flat behavior.
    if (mode != ProjectionMode::Flat || m_projectionMode == mode) {
        return;
    }
    m_projectionMode = mode;
    m_stack->setCurrentWidget(m_flatView);
    emit projectionModeChanged(mode);
}

bool MapDisplayWidget::globeAvailable() const
{
    return false;
}

void MapDisplayWidget::resetToHome()
{
    m_flatView->resetToHome();
}

void MapDisplayWidget::zoomIn()
{
    m_flatView->zoomIn();
}

void MapDisplayWidget::zoomOut()
{
    m_flatView->zoomOut();
}

} // namespace AetherSDR
