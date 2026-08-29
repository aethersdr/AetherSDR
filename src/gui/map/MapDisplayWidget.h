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
    QString globeUnavailableReason() const { return m_globeUnavailableReason; }

signals:
    void markerClicked(const MapDisplayWidget::Marker& marker);
    void projectionModeChanged(ProjectionMode mode);
    void globeAvailabilityChanged(bool available, const QString& reason);

public slots:
    void resetToHome();
    void zoomIn();
    void zoomOut();

private:
    void ensureGlobeView();
    void synchronizeFlatView();
    void synchronizeGlobeView();
    void handleGlobeUnavailable(const QString& reason);

    QStackedLayout* m_stack{nullptr};
    MapView* m_flatView{nullptr};
    GlobeMapView* m_globeView{nullptr};
    ProjectionMode m_projectionMode{ProjectionMode::Flat};
    QVector<Marker> m_markers;
    QVector<QPair<QString, QColor>> m_legendEntries;
    double m_homeLat{0.0};
    double m_homeLon{0.0};
    double m_homeSpanDegrees{30.0};
    QString m_homeLabel;
    bool m_hasHome{false};
    bool m_showHomeMarker{true};
    bool m_pathsVisible{true};
    bool m_terminatorVisible{false};
    bool m_flatViewDirty{false};
    bool m_globeViewDirty{false};
    bool m_globeAvailable{true};
    QString m_globeUnavailableReason;
};

} // namespace AetherSDR
