#pragma once

#include "CityLightsShading.h"
#include "MapView.h"
#include "WeatherRadarLoadingStatus.h"
#include "WeatherRadarPlaybackTimeline.h"
#include "WeatherRadarSource.h"
#include "WeatherRadarViewGeometry.h"

#include <QDateTime>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QRectF>
#include <QSet>
#include <QSize>
#include <QUrl>
#include <QVector>
#include <QWidget>

class QStackedLayout;
class QTimer;
class QNetworkAccessManager;
class QNetworkReply;
class QLabel;

namespace AetherSDR {

class GlobeMapView;
class CityLightsSource;
class WeatherRadarController;
class WeatherRadarLegend;

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
    ~MapDisplayWidget() override;

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
    void setCityLightsVisible(bool visible);
    // The host supplies the expanded credits; OSM stays visible on the map.
    void setDetailedAttributionVisible(bool visible);
    bool cityLightsVisible() const { return m_cityLightsVisible; }
    void setBasemapDarkEnabled(bool enabled);
    void setBasemapBrightness(int percent);
    void setCityLightsBrightness(int percent);
    void setCityLightsFaintLights(int percent);
    void setCityLightsWarmth(int percent);
    int cityLightsBrightness() const { return m_cityLightsBrightness; }
    void setRadarCoverageVisible(bool visible);
    void setRadarLegendVisible(bool visible);
    void setRadarLegendAtTop(bool atTop);
    void setWeatherRadarVisible(bool visible);
    void setWeatherRadarProvider(WeatherRadarSource::Provider provider);
    void setWeatherRadarRegions(int enabledProviders);
    void switchWeatherRadarSource(const WeatherRadarSource& source);
    bool weatherRadarVisible() const;
    void startWeatherRadarAnimation(int historyHours);
    void stopWeatherRadarAnimation();
    void setWeatherRadarPlaybackSpeed(int speedPercent);
    bool weatherRadarAnimating() const;
    void setLegend(const QVector<QPair<QString, QColor>>& entries);

    ProjectionMode projectionMode() const { return m_projectionMode; }
    void setProjectionMode(ProjectionMode mode);
    bool globeAvailable() const;
    QString globeUnavailableReason() const { return m_globeUnavailableReason; }

signals:
    void radarCoverageStatusChanged(const QString& status);
    void radarProviderStatusChanged(const QString& status);
    void cityLightsStatusChanged(const QString& status);
    void markerClicked(const MapDisplayWidget::Marker& marker);
    void projectionModeChanged(ProjectionMode mode);
    void globeAvailabilityChanged(bool available, const QString& reason);
    void weatherRadarAnimationStateChanged(bool playing);
    void weatherRadarTimelineLoadingChanged(bool loading);
    void weatherRadarFrameChanged(const QDateTime& frameTime, bool live);
    void weatherRadarAnimationError(const QString& message);

public slots:
    void resetToHome();
    void zoomIn();
    void zoomOut();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    friend class WeatherRadarLoadingTest;
    void ensureGlobeView();
    void synchronizeFlatView();
    void synchronizeGlobeView();
    void handleGlobeUnavailable(const QString& reason);
    void updateOverlayLoadingStatus();
    void refreshCityLightsView();
    void presentCityLights();
    void positionRadarLegend();

    bool m_detailedAttributionVisible{true};
    CityLightsSource* m_cityLightsSource{nullptr};
    bool m_cityLightsVisible{false};
    bool m_basemapDarkEnabled{false};
    int m_basemapBrightness{100};
    int m_cityLightsBrightness{CityLightsShading::kDefaultBrightness};
    int m_cityLightsFaintLights{CityLightsShading::kDefaultFaintLights};
    int m_cityLightsWarmth{CityLightsShading::kDefaultWarmth};
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
    QLabel* m_weatherRadarLoadingLabel{nullptr};
    QString m_cityLightsLoadingText;
    QString m_overlayLoadingAnnouncement;
    QTimer* m_cityLightsLoadingTimer{nullptr};
    QString m_weatherRadarLoadingText;
    QString m_weatherRadarLoadingAnnouncement;
    WeatherRadarController* m_weatherRadar{nullptr};
    WeatherRadarLegend* m_radarLegend{nullptr};
    bool m_radarLegendAtTop{false};
    bool m_flatViewDirty{false};
    bool m_globeViewDirty{false};
    bool m_globeAvailable{true};
    QString m_globeUnavailableReason;
};

} // namespace AetherSDR
