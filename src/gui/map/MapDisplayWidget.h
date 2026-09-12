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
    void setWeatherRadarVisible(bool visible);
    void setWeatherRadarProvider(WeatherRadarSource::Provider provider);
    void setWeatherRadarRegions(int enabledProviders);
    void switchWeatherRadarSource(const WeatherRadarSource& source);
    bool weatherRadarVisible() const { return m_weatherRadarVisible; }
    void startWeatherRadarAnimation(int historyHours);
    void stopWeatherRadarAnimation();
    void setWeatherRadarPlaybackSpeed(int speedPercent);
    bool weatherRadarAnimating() const
    {
        return m_weatherRadarPlaybackRequested;
    }
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

private:
    friend class WeatherRadarLoadingTest;
    struct CachedWeatherRadarFrame {
        QByteArray bytes;
        QImage decodedImage;
        QDateTime frameTime;
        QString sourceId;
        WeatherRadarViewGeometry geometry;
        qint64 lastAccessMs{0};
    };

    void ensureGlobeView();
    void synchronizeFlatView();
    void synchronizeGlobeView();
    void handleGlobeUnavailable(const QString& reason);
    void applyWeatherRadarSource(const WeatherRadarSource& source);
    WeatherRadarSource playbackWeatherRadarSource() const;
    void requestWeatherRadarTimeline(int historyHours, bool background = false);
    void appendWeatherRadarObservations(const QVector<WeatherRadarObservation>& observations);
    bool useWeatherRadarTimeline(const QByteArray& payload,
                                 int historyHours);
    void bufferWeatherRadarFrames(bool retainPlayback = false);
    WeatherRadarViewGeometry weatherRadarCurrentView() const;
    QRectF weatherRadarRendererBounds(const QRectF& bounds) const;
    void decodeWeatherRadarDownload(int index, const QByteArray& bytes,
        const QString& key, const WeatherRadarViewGeometry& geometry, bool trustedCache = false);
    void validateWeatherRadarEmptyFrame(int generation, int index,
        const QString& key, const CachedWeatherRadarFrame& frame);
    void acceptWeatherRadarDownload(int generation, int index,
        const QString& key, const CachedWeatherRadarFrame& frame);
    void finishWeatherRadarDownloadBatch();
    void updateWeatherRadarLoadingStatus();
    void updateOverlayLoadingStatus();
    void retryWeatherRadarHistory();
    void requestNextWeatherRadarBufferedFrames();
    void tryStartWeatherRadarBufferedPrefix();
    void finalizeWeatherRadarBuffering();
    void applyFinalizedWeatherRadarBuffering();
    void cancelWeatherRadarFrameRequests();
    void scheduleWeatherRadarDecode(int index);
    void handleWeatherRadarDecodeFailure(int index);
    int weatherRadarPlaybackFrameCount() const;
    void ensureWeatherRadarDecodeAhead(int index);
    void pruneWeatherRadarDecodedImages(int index, int frameCount);
    void tryStartWeatherRadarPlayback();
    void rebufferWeatherRadarPlayback();
    void startWeatherRadarPlaybackClock(const QDateTime& frameTime);
    void handleWeatherRadarPlaybackPresented(quint64 presentationSequence);
    void updateWeatherRadarPlayback();
    bool tryPresentWeatherRadarElapsed(qint64 candidateElapsedMs);
    bool presentWeatherRadarFrame(int index, quint64 presentationSequence);
    void preloadNextWeatherRadarFrame();
    void cancelWeatherRadarTimelineRequest();
    void resetWeatherRadarAnimation(bool returnToLive);
    void pruneWeatherRadarCache();
    void refreshCityLightsView();
    void presentCityLights();

    void presentRadarSites();
    QVector<RadarSite> m_radarSiteCatalogs[2];
    QSet<QNetworkReply*> m_radarSiteReplies;
    bool m_radarCoverageVisible{false};
    bool m_detailedAttributionVisible{true};
    QDateTime m_radarSitesRequestedAt;
    bool m_radarSiteFailed[2]{false, false};
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
    bool m_weatherRadarVisible{false};
    QTimer* m_weatherRadarTimer{nullptr};
    QTimer* m_weatherRadarPlaybackTimer{nullptr};
    QTimer* m_weatherRadarRebufferTimer{nullptr};
    QNetworkAccessManager* m_weatherRadarNetwork{nullptr};
    QNetworkReply* m_weatherRadarTimelineReply{nullptr};
    WeatherRadarSource m_weatherRadarSource;
    int m_weatherRadarPlaybackProviders{-1};
    QVector<QDateTime> m_weatherRadarFrames;
    QVector<QDateTime> m_weatherRadarFrameSampleTimes;
    QVector<QVector<qint64>> m_weatherRadarFrameRasterIds;
    QVector<QByteArray> m_weatherRadarBufferedBytes;
    QVector<QUrl> m_weatherRadarFrameUrls;
    QVector<WeatherRadarViewGeometry> m_weatherRadarFrameRequestGeometry;
    QVector<QString> m_weatherRadarFrameCacheKeys;
    // Canonical EPSG:3857, even when accepted while QGeoView is active.
    // The projection switch retains these images; reflect only at draw time.
    QVector<QRectF> m_weatherRadarFrameBounds;
    QSet<int> m_weatherRadarDownloadDecodePending;
    QSet<int> m_weatherRadarDownloadReady;
    QSet<int> m_weatherRadarDownloadFailed;
    // Stable timestamps, not indexes: active indexes change only at loop wrap.
    QSet<QDateTime> m_weatherRadarExpiredFrames;
    // Retryable observations survive compaction of the playable timeline.
    QSet<QDateTime> m_weatherRadarRetryFrames;
    WeatherRadarViewGeometry m_weatherRadarRequestedView;
    bool m_weatherRadarDetailRefresh{false};
    qint64 m_weatherRadarPresentedImageKey{0};
    QLabel* m_weatherRadarLoadingLabel{nullptr};
    QTimer* m_weatherRadarLoadingTimer{nullptr};
    QElapsedTimer m_weatherRadarLoadingElapsed;
    WeatherRadarLoadingStatus m_weatherRadarLoadingStatus;
    QString m_weatherRadarLoadingAnnouncement;
    QString m_weatherRadarLoadingText;
    QString m_cityLightsLoadingText;
    QString m_overlayLoadingAnnouncement;
    QTimer* m_cityLightsLoadingTimer{nullptr};
    QVector<int> m_weatherRadarSegmentDurationsMs;
    QHash<int, QImage> m_weatherRadarDecodedImages;
    QSet<int> m_weatherRadarDecodePending;
    QVector<int> m_weatherRadarActiveSegmentDurationsMs;
    int m_weatherRadarPlaybackSpeedPercent{100};
    int m_weatherRadarHistoryHours{1};
    QVector<int> m_weatherRadarBufferQueue;
    QSet<QNetworkReply*> m_weatherRadarFrameReplies;
    QRectF m_weatherRadarPlaybackBounds;
    QRectF m_weatherRadarPlaybackRequestBounds;
    QSize m_weatherRadarPlaybackSize;
    QHash<QString, CachedWeatherRadarFrame> m_weatherRadarFrameCache;
    QByteArray m_weatherRadarTimelineCache;
    QDateTime m_weatherRadarTimelineCachedAt;
    int m_activeWeatherRadarFrameRequests{0};
    int m_weatherRadarBufferGeneration{0};
    int m_weatherRadarPlayableFrameCount{0};
    bool m_weatherRadarNetworkBufferComplete{false};
    bool m_weatherRadarNetworkRequestsComplete{false};
    bool m_weatherRadarBufferFinalizationPending{false};
    int m_weatherRadarFrameIndex{-1};
    int m_weatherRadarPresentedFromIndex{-1};
    int m_weatherRadarPresentedToIndex{-1};
    QElapsedTimer m_weatherRadarPlaybackClock;
    WeatherRadarPlaybackCadence m_weatherRadarPlaybackCadence;
    bool m_weatherRadarPlaybackClockPending{false};
    bool m_weatherRadarPlaybackPreloadPending{false};
    bool m_weatherRadarPlaybackRequested{false};
    bool m_weatherRadarAnimating{false};
    bool m_weatherRadarTimelineLoading{false};
    bool m_weatherRadarTimelineFailed{false};
    bool m_weatherRadarRebuffering{false};
    bool m_flatViewDirty{false};
    bool m_globeViewDirty{false};
    bool m_globeAvailable{true};
    QString m_globeUnavailableReason;
};

} // namespace AetherSDR
