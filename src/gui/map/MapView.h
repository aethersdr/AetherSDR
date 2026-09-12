#pragma once
#include "RadarCoverage.h"

#include "WeatherRadarSource.h"

#include <QWidget>
#include <QTimer>
#include <QColor>
#include <QPointer>
#include <QVector>

#include <QGeoView/QGVGlobal.h>

class QGVMap;
class QGVItem;
class QGVLayer;
class QAbstractAnimation;
class QLabel;
class QOpenGLWidget;
class QToolButton;
class QVariantAnimation;

namespace AetherSDR {

class DarkBasemapLayer;

class MapMarkerItem;
class MapMarkerBatchItem;
class MapPathBatchItem;
class MapTerminatorItem;
class CityLightsItem;
class WeatherRadarPlaybackItem;
class WeatherRadarTileLayer;

// Reusable OpenStreetMap slippy-map widget (#mapping-engine).
//
// Wraps the vendored QGeoView QGVMap with:
//   * A policy-compliant OSM tile layer — shared QNetworkAccessManager with
//     a QNetworkDiskCache (HTTP cache headers honored, OSM requires >= 7
//     days) and an app-identifying User-Agent.
//   * Keyboard navigation: arrow keys pan, +/- (and =) zoom, Home recenters
//     on the home position (the radio's GPS fix for the PSK Reporter map).
//   * A simple marker API (MapView::Marker) used by the PSK Reporter map
//     and, in the future, the AetherModem APRS tab.
//   * The mandatory "© OpenStreetMap contributors" attribution overlay.
class MapView : public QWidget {
    Q_OBJECT

public:
    enum class ViewportMode {
        Raster,
        OpenGlIfAvailable
    };

    struct Marker {
        double  lat{0.0};
        double  lon{0.0};
        QString label;       // short text drawn next to the dot
        QString tooltip;     // hover detail
        QColor  color{Qt::red};
        bool    isHome{false};  // drawn as a distinct station marker
        bool    isMonitor{false}; // larger active-receiver marker
        bool    pathEnabled{true};
        bool    hasPathOrigin{false};
        double  pathFromLat{0.0};
        double  pathFromLon{0.0};
        QString pathGroup;       // non-empty groups related hover paths
        bool    hoverShowsPathGroup{false};
        QString clickInfo;      // rich text shown on click (empty = none)
    };

    explicit MapView(
        QWidget* parent = nullptr,
        ViewportMode viewportMode = ViewportMode::Raster);

    // A narrow diagnostic seam for automation and platform smoke tests. The
    // answer changes to false if the requested OpenGL viewport cannot create
    // a context and MapView has fallen back to the raster viewport.
    bool openGlViewportActive() const;

    // Home position (e.g. radio GPS fix). Home key / resetToHome() recenters
    // here. Also draws/updates the home station marker when showMarker.
    void setHomePosition(double lat, double lon, const QString& label = {},
                         bool showMarker = true);
    // Latitude span used by resetToHome(); longitude uses twice this span to
    // fit a typical landscape widget. The default remains the broad HF-path
    // view used by PSK Reporter, while compact location maps can request a
    // neighbourhood-scale view.
    void setHomeSpanDegrees(double spanDegrees);
    bool hasHomePosition() const { return m_hasHome; }

    void setMarkers(const QVector<Marker>& markers);
    void clearMarkers();

    // Great-circle paths from home to every marker.
    void setPathsVisible(bool visible);
    bool pathsVisible() const { return m_pathsVisible; }

    void setDayNightTerminatorVisible(bool visible);
    bool dayNightTerminatorVisible() const;
    void setCityLightsVisible(bool visible);
    void setCityLightsImage(const QImage& image, const QRectF& bounds);
    void setBasemapDarkEnabled(bool enabled);
    void setBasemapBrightness(int percent);
    void setCityLightsBrightness(int percent);
    void setRadarSites(const QVector<RadarSite>& sites, bool visible);
    void setDetailedAttributionVisible(bool visible);
    void setWeatherRadarVisible(bool visible);
    bool weatherRadarVisible() const;
    int pendingWeatherRadarRequests() const;
    bool weatherRadarLoadFailed() const;
    void refreshWeatherRadar();
    void setWeatherRadarSource(const WeatherRadarSource& source);
    QRectF weatherRadarPlaybackBounds() const;
    QSize weatherRadarPlaybackSize() const;
    // Atomic presentation of a single original NOAA observation.
    bool showWeatherRadarPlaybackFrame(
        const QImage& image, const QDateTime& frameTime, const QRectF& bounds);
    void acknowledgeWeatherRadarPlaybackFrame(quint64 presentationSequence);
    void preloadWeatherRadarPlaybackFrame(const QImage& image,
                                          const QDateTime& frameTime,
                                          const QRectF& bounds);
    void clearWeatherRadarPlayback();

    // Color/label legend chip, lower-left. Empty list hides it.
    void setLegend(const QVector<QPair<QString, QColor>>& entries);

    double homeLat() const { return m_homeLat; }
    double homeLon() const { return m_homeLon; }

    QGVMap* map() const { return m_map; }

signals:
    void imageOverlayViewChanged();
    void markerClicked(const MapView::Marker& marker);
    void weatherRadarFrameLoaded(const QDateTime& frameTime);
    void weatherRadarPlaybackPresented(quint64 presentationSequence);
    void weatherRadarPlaybackFramePreloaded(const QDateTime& frameTime);
    void weatherRadarPlaybackInvalidated();

public slots:
    void resetToHome();
    void zoomIn();
    void zoomOut();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    // Watches the inner QGraphicsView viewport for mouse-move so the hover
    // card can appear instantly (QGeoView's own tooltip path is delayed and
    // its mapMouseMove signal doesn't fire reliably for plain hovering).
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // Install the process-wide tile network manager (disk cache + UA) on
    // first use. Safe to call repeatedly.
    static void ensureTileNetworkManager();

    void pan(double dxFraction, double dyFraction);
    void animateZoom(double factor);
    QToolButton* makeOverlayButton(const QString& text, const QString& tip);
    void layoutOverlayButtons();
    void rebuildPaths();
    void updateHoverPath(int markerIndex);
    void clearHoverPath();
    void clampMinZoomToViewport();
    // How many world copies either side of the base one the markers and paths
    // must be replicated into for the widest viewport this widget can reach.
    // The world repeats horizontally, but each item can only be in one place,
    // so a copy per visible world is what keeps spots on screen after the
    // camera crosses the antimeridian.
    int requiredWorldCopyRange() const;
    // Re-create every marker and path at the current copy range. Only called
    // when requiredWorldCopyRange() actually changes, which needs a resize.
    void rebuildWorldCopies();
    void rebuildHomeMarkers();
    Marker homeMarkerData() const;
    // Instant hover tooltip driven by mouse-move (QGeoView's built-in
    // tooltip waits for the OS hover delay, which is too slow here).
    void showHoverTooltip(const QPointF& projPos);
    void handleWeatherRadarFrameReady(WeatherRadarTileLayer* layer,
                                      const QDateTime& frameTime);
    void configureViewportInput(QWidget* viewport);
    void fallBackToRasterViewport();
    void updateAttributionStyle();
    void updateMapAttribution();

    class RadarCoverageItem* m_radarCoverageItem{nullptr};
    DarkBasemapLayer* m_basemapLayer{nullptr};
    QGVItem* m_basemapDimmer{nullptr};
    QGVMap*  m_map{nullptr};
    QGVLayer* m_markerLayer{nullptr};
    QGVLayer* m_terminatorLayer{nullptr};
    CityLightsItem* m_cityLightsItem{nullptr};
    bool m_cityLightsVisible{false};
    QGVLayer* m_weatherRadarPlaybackLayer{nullptr};
    WeatherRadarTileLayer* m_weatherRadarLayer{nullptr};
    WeatherRadarTileLayer* m_weatherRadarNextLayer{nullptr};
    WeatherRadarSource m_weatherRadarSource;
    QString m_pendingWeatherRadarFrameId;
    QVariantAnimation* m_weatherRadarTransition{nullptr};
    WeatherRadarPlaybackItem* m_weatherRadarPlaybackItem{nullptr};
    bool m_weatherRadarPlaybackActive{false};
    bool m_weatherRadarEnabled{false};
    QLabel* m_attribution{nullptr};
    bool m_detailedAttributionVisible{true};
    MapTerminatorItem* m_terminatorItem{nullptr};
    QTimer* m_terminatorTimer{nullptr};
    QVector<MapMarkerItem*> m_homeMarkers;
    int m_hoverMarkerIndex{-1};
    QLabel* m_hoverCard{nullptr};
    MapMarkerBatchItem* m_markerBatch{nullptr};
    QVector<Marker> m_markerData;
    MapPathBatchItem* m_pathBatch{nullptr};
    MapPathBatchItem* m_hoverPathBatch{nullptr};
    int m_hoverPathMarkerIndex{-1};
    bool m_pathsVisible{true};
    QLabel* m_legend{nullptr};

    double m_homeLat{0.0};
    double m_homeLon{0.0};
    QString m_homeLabel;
    bool   m_hasHome{false};
    bool   m_homeMarkerShown{false};
    bool   m_firstShow{true};
    int    m_worldCopyRange{1};
    double m_homeSpanDeg{30.0};

    // Zoom / recenter overlay buttons (upper-right).
    QToolButton* m_zoomInBtn{nullptr};
    QToolButton* m_zoomOutBtn{nullptr};
    QToolButton* m_homeBtn{nullptr};
    QPointer<QAbstractAnimation> m_zoomAnimation;
    QPointer<QOpenGLWidget> m_openGlViewport;
    bool m_openGlViewportChecked{false};

    // Sonar pulse on the home marker: a short ring animation fired every
    // few seconds. The animation only runs for its ~1s duration, so the
    // idle cost is one timer tick every 3 s.
    QTimer* m_pulseTimer{nullptr};
    QVariantAnimation* m_pulseAnim{nullptr};
};

} // namespace AetherSDR
