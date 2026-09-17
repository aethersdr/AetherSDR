#pragma once

#include <QDateTime>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QUrl>
#include <QVector>
#include <optional>

namespace AetherSDR {

struct WeatherRadarObservation {
    QDateTime frameTime;
    QDateTime sampleTime;
    QVector<qint64> rasterIds;
};

// Provider-neutral description of one near-real-time weather radar frame.
// Live NOAA exports do not expose an observation timestamp, so their
// frameTime is only a refresh bucket. Historical sources preserve the exact
// timestamp reported by NOAA's time-enabled image service.
class WeatherRadarSource final {
public:
    enum class Provider {
        NoaaMrms,
        Eccc,
        Opera,
        Composite,
        LibreWxr
    };

    enum class FrameMode {
        Latest,
        Historical
    };

    explicit WeatherRadarSource(
        Provider provider = Provider::NoaaMrms,
        const QDateTime& frameTime = QDateTime::currentDateTimeUtc(),
        FrameMode mode = FrameMode::Latest,
        const QDateTime& sampleTime = {});

    static WeatherRadarSource currentNoaaFrame();
    static WeatherRadarSource composite(int enabledProviders);
    int enabledProviders() const { return m_enabledProviders; }
    WeatherRadarSource latestFrame() const;
    WeatherRadarSource historicalFrame(const QDateTime& time,
        const QDateTime& sample = {}, const QVector<qint64>& ids = {}) const;
    QUrl timelineUrl() const;
    WeatherRadarSource playbackSourceForTimeline(const QByteArray& bytes) const;
    QVector<WeatherRadarObservation> parseTimeline(const QByteArray& bytes, int hours) const;
    QString productDescription() const;
    static QUrl operaTimelineUrl();
    static QUrl operaFrameUrl(const QDateTime& time);
    static WeatherRadarSource historicalNoaaFrame(
        const QDateTime& frameTime,
        const QDateTime& sampleTime = {},
        const QVector<qint64>& rasterIds = {});
    static QUrl noaaTimelineUrl();
    static QUrl noaaRasterAvailabilityUrl(const QVector<qint64>& rasterIds);
    // nullopt means an untrustworthy response, not expired radar.
    static std::optional<bool> parseNoaaRasterAvailability(
        const QByteArray& json, const QVector<qint64>& rasterIds);
    static QVector<WeatherRadarObservation> parseNoaaTimeline(
        const QByteArray& json, int historyHours);
    // QGeoView's screen-oriented EPSG:3857 projection stores north as
    // negative Y. ArcGIS exportImage uses conventional Web Mercator, where
    // north is positive Y. Convert a QGeoView camera rectangle before using
    // it as an image-service bbox; the returned image is still displayed in
    // the original QGeoView rectangle.
    static QRectF conventionalBoundsFromQgv(const QRectF& qgvBounds);

    Provider provider() const { return m_provider; }
    QDateTime frameTime() const { return m_frameTime; }
    FrameMode frameMode() const { return m_frameMode; }
    QString frameId() const;
    QString attribution() const;
    int minimumZoom() const { return 0; }
    int maximumZoom() const { return 12; }
    int tilePixelSize() const;
    QUrl tileUrl(int zoom, int x, int y) const;
    QUrl imageUrl(const QRectF& webMercatorBounds,
                  const QSize& pixelSize) const;

private:
    Provider m_provider{Provider::NoaaMrms};
    int m_enabledProviders{7};
    QDateTime m_frameTime;
    QDateTime m_sampleTime;
    QVector<qint64> m_rasterIds;
    FrameMode m_frameMode{FrameMode::Latest};
};

} // namespace AetherSDR
