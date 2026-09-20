#pragma once
#include "MapView.h"
#include "WeatherRadarFrame.h"
#include "WeatherRadarLoadingStatus.h"
#include "WeatherRadarPlaybackTimeline.h"
#include "WeatherRadarViewGeometry.h"
#include <QObject>
#include <QElapsedTimer>
#include <QSet>
class QTimer;
class QNetworkAccessManager;
class QNetworkReply;
namespace AetherSDR {
class GlobeMapView;
// Owns weather network, cache and animation state across renderer changes.
// Renderers remain owned by MapDisplayWidget and outlive this controller.
class WeatherRadarController : public QObject {
    Q_OBJECT
public:
    explicit WeatherRadarController(MapView* flatView, QObject* parent = nullptr);
    ~WeatherRadarController() override;
    void attachGlobe(GlobeMapView* view);
    void setGlobeActive(bool active);
    void synchronizeRenderer(bool globe);
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
signals:
    void displayedProvidersChanged(int providers);
    void radarCoverageStatusChanged(const QString& status);
    void radarProviderStatusChanged(const QString& status);
    void weatherRadarAnimationStateChanged(bool playing);
    void weatherRadarTimelineLoadingChanged(bool loading);
    void weatherRadarFrameChanged(const QDateTime& frameTime, bool live);
    void weatherRadarAnimationError(const QString& message);
    void loadingStatusChanged(const QString& text, const QString& announcement);
private:
    friend class WeatherRadarLoadingTest;
    void publishDisplayedProviders();
    int m_lastDisplayedProviders{-1};
    int m_flatDisplayedProviders{0};
    int m_globeDisplayedProviders{0};
    int m_playbackDisplayedProviders{0};
    bool m_presentingPlayback{false};
    MapView* m_flatView;
    GlobeMapView* m_globeView{nullptr};
    bool m_globeActive{false};
    bool m_flatSourceDirty{false};
    bool m_globeSourceDirty{false};
    QVector<WeatherRadarFrame> m_frames;
    template<typename T> QVector<T> frameValues(T WeatherRadarFrame::*member) const {
        QVector<T> values;
        values.reserve(m_frames.size());
        for (const WeatherRadarFrame& frame : m_frames) { values.append(frame.*member); }
        return values;
    }
    struct CachedWeatherRadarFrame {
        QByteArray bytes;
        QImage decodedImage;
        QDateTime frameTime;
        QString sourceId;
        WeatherRadarViewGeometry geometry;
        qint64 lastAccessMs{0};
    };

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
    void presentRadarSites();
    QVector<RadarSite> m_radarSites;
    bool m_radarCoverageVisible{false};
    bool m_weatherRadarVisible{false};
    QTimer* m_weatherRadarTimer{nullptr};
    QTimer* m_weatherRadarPlaybackTimer{nullptr};
    QTimer* m_weatherRadarRebufferTimer{nullptr};
    QNetworkAccessManager* m_weatherRadarNetwork{nullptr};
    QNetworkReply* m_weatherRadarTimelineReply{nullptr};
    WeatherRadarSource m_weatherRadarSource;
    int m_weatherRadarPlaybackProviders{-1};
    // Canonical EPSG:3857, even when accepted while QGeoView is active.
    // The projection switch retains these images; reflect only at draw time.
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
    QTimer* m_weatherRadarLoadingTimer{nullptr};
    QElapsedTimer m_weatherRadarLoadingElapsed;
    WeatherRadarLoadingStatus m_weatherRadarLoadingStatus;
    QString m_weatherRadarLoadingAnnouncement;
    QString m_weatherRadarLoadingText;
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
};
}
