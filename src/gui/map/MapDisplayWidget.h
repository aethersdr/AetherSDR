#pragma once

#include "MapView.h"

#include <QWidget>

class QStackedLayout;

namespace AetherSDR {

class GlobeMapView;

// Projection-neutral facade for map consumers that can switch renderers.
// The GPS dialog continues to use MapView directly; PSK Reporter uses this
// facade so its data/filter logic remains independent of the selected map
// projection.
class MapDisplayWidget : public QWidget {
    Q_OBJECT

public:
    using Marker = MapView::Marker;

    enum class ProjectionMode {
        Flat,
        Globe
    };

    explicit MapDisplayWidget(QWidget* parent = nullptr);

    void setHomePosition(double lat, double lon, const QString& label = {},
                         bool showMarker = true);
    void setHomeSpanDegrees(double spanDegrees);
    bool hasHomePosition() const;
    double homeLat() const;
    double homeLon() const;

    void setMarkers(const QVector<Marker>& markers);
    void clearMarkers();
    void setPathsVisible(bool visible);
    bool pathsVisible() const;
    void setDayNightTerminatorVisible(bool visible);
    bool dayNightTerminatorVisible() const;
    void setLegend(const QVector<QPair<QString, QColor>>& entries);

    ProjectionMode projectionMode() const { return m_projectionMode; }
    void setProjectionMode(ProjectionMode mode);
    bool globeAvailable() const;

signals:
    void markerClicked(const MapDisplayWidget::Marker& marker);
    void projectionModeChanged(ProjectionMode mode);

public slots:
    void resetToHome();
    void zoomIn();
    void zoomOut();

private:
    QStackedLayout* m_stack{nullptr};
    MapView* m_flatView{nullptr};
    GlobeMapView* m_globeView{nullptr};
    ProjectionMode m_projectionMode{ProjectionMode::Flat};
};

} // namespace AetherSDR
