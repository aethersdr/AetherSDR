#include "CityLightsSource.h"
#include "MapProviderNetworkAccessManager.h"
#include "SolarTerminator.h"

#include <array>

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFutureWatcher>
#include <QImageReader>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QtConcurrent/QtConcurrentRun>

namespace AetherSDR {
namespace {
constexpr int kMaximumDimension = 4096;
constexpr qint64 kMaximumBytes = 16 * 1024 * 1024;
// GIBS GoogleMapsCompatible_Level8: 256 pixels * 2^8 across the world.
constexpr double kNativePixelMetres = kRadarWorldWidth / 65536.0;
}

CityLightsSource::CityLightsSource(QObject* parent, QNetworkAccessManager* network)
    : QObject(parent), m_network(network != nullptr ? network : new MapProviderNetworkAccessManager(this))
{
    if (network == nullptr) {
        auto* cache = new QNetworkDiskCache(m_network);
        cache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                                + QStringLiteral("/nasa-night-lights-2016"));
        cache->setMaximumCacheSize(64 * 1024 * 1024);
        m_network->setCache(cache);
    }
    m_debounce.setSingleShot(true);
    // Coarse timers may fire early and miss the provider cooldown plus jitter.
    m_debounce.setTimerType(Qt::PreciseTimer);
    connect(&m_debounce, &QTimer::timeout, this, &CityLightsSource::requestImage);
    m_clock.setInterval(60 * 1000);
    connect(&m_clock, &QTimer::timeout, this, &CityLightsSource::renderImage);
}

CityLightsSource::~CityLightsSource()
{
    cancelRequest();
}

WeatherRadarViewGeometry CityLightsSource::boundedView(const WeatherRadarViewGeometry& view)
{
    const QRectF& b = view.bounds;
    const QRectF world(-kRadarMercatorExtent, -kRadarMercatorExtent,
                       kRadarWorldWidth, kRadarWorldWidth);
    if (!std::isfinite(b.left()) || !std::isfinite(b.top())
        || !std::isfinite(b.right()) || !std::isfinite(b.bottom())
        || b.isEmpty() || view.size.isEmpty() || !world.contains(b)) {
        return {};
    }
    const QSize size(
        std::clamp(view.size.width(), 1, std::min(kMaximumDimension,
            std::max(1, int(std::floor(b.width() / kNativePixelMetres))))),
        std::clamp(view.size.height(), 1, std::min(kMaximumDimension,
            std::max(1, int(std::floor(b.height() / kNativePixelMetres))))));
    return weatherRadarPaddedView({b, size}, kMaximumDimension);
}

QUrl CityLightsSource::imageUrl(const WeatherRadarViewGeometry& view)
{
    if (view.bounds.isEmpty() || view.size.isEmpty()) {
        return {};
    }
    QUrl url(QStringLiteral("https://gibs.earthdata.nasa.gov/wms/epsg3857/best/wms.cgi"));
    QUrlQuery query;
    query.addQueryItem("SERVICE", "WMS");
    query.addQueryItem("VERSION", "1.1.1");
    query.addQueryItem("REQUEST", "GetMap");
    query.addQueryItem("LAYERS", "VIIRS_Night_Lights");
    query.addQueryItem("TIME", "2016-01-01");
    query.addQueryItem("STYLES", "");
    query.addQueryItem("FORMAT", "image/png");
    query.addQueryItem("TRANSPARENT", "TRUE");
    query.addQueryItem("SRS", "EPSG:3857");
    query.addQueryItem("BBOX", QStringLiteral("%1,%2,%3,%4")
        .arg(view.bounds.left(), 0, 'f', 3).arg(view.bounds.top(), 0, 'f', 3)
        .arg(view.bounds.right(), 0, 'f', 3).arg(view.bounds.bottom(), 0, 'f', 3));
    query.addQueryItem("WIDTH", QString::number(view.size.width()));
    query.addQueryItem("HEIGHT", QString::number(view.size.height()));
    url.setQuery(query);
    return url;
}

QImage CityLightsSource::decode(const QByteArray& bytes, const QSize& expectedSize)
{
    if (bytes.isEmpty() || bytes.size() > kMaximumBytes || expectedSize.isEmpty()
        || expectedSize.width() > kMaximumDimension || expectedSize.height() > kMaximumDimension) {
        return {};
    }
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "PNG");
    // Validate BEFORE allocating decoded pixels; XML errors are not images.
    if (reader.size() != expectedSize) {
        return {};
    }
    QImage image = reader.read().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    // GIBS' lights-only PNG still contains opaque black land pixels. Treat
    // emitted light as coverage: keep its premultiplied RGB, but reduce alpha
    // to the brightest channel. Black then contributes nothing, and faint
    // lights do not paint dark rectangles over the street map. This is a
    // display blend, not a quantitative radiance measurement.
    for (int y = 0; y < image.height(); ++y) {
        QRgb* pixels = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb p = pixels[x];
            pixels[x] = qRgba(qRed(p), qGreen(p), qBlue(p),
                              std::max({qRed(p), qGreen(p), qBlue(p)}));
        }
    }
    return image;
}

QImage CityLightsSource::nightImage(const QImage& source, const QRectF& bounds,
                                   const QDateTime& time, bool nightOnly, int faintLights, int warmth)
{
    if ((!nightOnly && faintLights == 0 && warmth == 0) || source.isNull()) {
        return source;
    }
    QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    // Lift dim lights without clipping bright city centers or changing hue.
    // A lookup table avoids a power operation for every image pixel.
    const double warm = std::clamp(warmth, 0, 100) / 100.0;
    const double gamma = CityLightsShading::faintLightsGamma(faintLights);
    std::array<double, 256> gains{};
    for (int value = 1; value < 256; ++value) {
        gains[value] = std::pow(value / 255.0, gamma) * 255.0 / value;
    }
    const SolarTerminator::Position sun = SolarTerminator::positionAt(time);
    QVector<double> hourCosines(image.width());
    for (int x = 0; x < image.width(); ++x) {
        const double longitude = (bounds.left() + (x + 0.5) * bounds.width() / image.width())
            / kRadarMercatorExtent * M_PI;
        hourCosines[x] = std::cos(longitude - sun.subsolarLonRad);
    }
    const double sunSin = std::sin(sun.declinationRad);
    const double twilightSine = CityLightsShading::twilightSine();
    const double sunCos = std::cos(sun.declinationRad);
    for (int y = 0; y < image.height(); ++y) {
        const double northing = bounds.bottom() - (y + 0.5) * bounds.height() / image.height();
        const double latitude = std::atan(std::sinh(northing / kRadarMercatorExtent * M_PI));
        const double latitudeSin = std::sin(latitude);
        const double latitudeCos = std::cos(latitude);
        QRgb* pixels = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const double elevation = latitudeSin * sunSin + latitudeCos * sunCos * hourCosines[x];
            // Smooth civil twilight: absent at the horizon, full at
            // -kCivilTwilightDegrees.
            const double t = std::clamp(-elevation / twilightSine, 0.0, 1.0);
            const QRgb p = pixels[x];
            const double amount = (nightOnly ? t * t * (3.0 - 2.0 * t) : 1.0)
                * gains[qAlpha(p)];
            pixels[x] = qRgba(qRound(qRed(p) * amount),
                              qRound(qGreen(p) * amount * (1.0 - CityLightsShading::kWarmthGreenLoss * warm)),
                              qRound(qBlue(p) * amount * (1.0 - CityLightsShading::kWarmthBlueLoss * warm)),
                              qRound(qAlpha(p) * amount));
        }
    }
    return image;
}

void CityLightsSource::setWarmth(int percent)
{
    const int value = std::clamp(percent, 0, 100);
    if (value == m_warmth) {
        return;
    }
    m_warmth = value;
    renderImage();
}

void CityLightsSource::setFaintLights(int percent)
{
    const int value = std::clamp(percent, 0, 100);
    if (value == m_faintLights) {
        return;
    }
    m_faintLights = value;
    renderImage();
}

void CityLightsSource::setEnabled(bool enabled)
{
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    if (!enabled) {
        m_debounce.stop();
        m_clock.stop();
        cancelRequest();
        emit statusChanged({});
        return;
    }
    renderImage();
    m_debounce.start(0);
    if (m_nightOnly) {
        m_clock.start();
    }
}

void CityLightsSource::setView(const WeatherRadarViewGeometry& view)
{
    const WeatherRadarViewGeometry next = boundedView(view);
    if (next.bounds == m_view.bounds && next.size == m_view.size) {
        return;
    }
    m_view = next;
    if (m_loaded.covers(m_view)) {
        cancelRequest();
        m_debounce.stop();
        emit statusChanged({});
        return;
    }
    if (m_enabled) {
        m_debounce.start(400);
    }
}

void CityLightsSource::setNightOnly(bool nightOnly)
{
    if (m_nightOnly == nightOnly) {
        return;
    }
    m_nightOnly = nightOnly;
    if (m_enabled && nightOnly) {
        m_clock.start();
    } else {
        m_clock.stop();
    }
    renderImage();
}

void CityLightsSource::cancelRequest()
{
    ++m_generation;
    if (m_reply != nullptr) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void CityLightsSource::requestImage()
{
    if (!m_enabled || m_view.bounds.isEmpty() || m_loaded.covers(m_view)) {
        return;
    }
    if (m_reply != nullptr && m_requested.covers(m_view)) {
        return;
    }
    cancelRequest();
    m_requested = m_view;
    const WeatherRadarViewGeometry requested = m_requested;
    const quint64 generation = m_generation;
    QNetworkRequest request(imageUrl(requested));
    request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    request.setRawHeader("User-Agent", QByteArray("AetherSDR/")
        + QCoreApplication::applicationVersion().toUtf8() + " (NASA city lights map overlay)");
    m_reply = m_network->get(request);
    QNetworkReply* reply = m_reply;
    reply->setReadBufferSize(kMaximumBytes + 1);
    emit statusChanged(tr("Loading city lights…"));
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > kMaximumBytes) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, requested, generation] {
        const bool valid = reply->error() == QNetworkReply::NoError
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
            && reply->bytesAvailable() <= kMaximumBytes;
        const QByteArray bytes = valid ? reply->read(kMaximumBytes + 1) : QByteArray{};
        reply->deleteLater();
        m_reply = nullptr;
        auto* watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this,
                [this, watcher, requested, generation, valid] {
            const QImage image = watcher->result();
            watcher->deleteLater();
            if (generation != m_generation || !m_enabled) {
                return;
            }
            if (image.isNull()) {
                // A provider can return an XML error with HTTP 200. Do not
                // replay that cached error on every retry for this viewport.
                // Transport errors and local denials contain no image to judge.
                if (valid && m_network->cache() != nullptr) {
                    m_network->cache()->remove(imageUrl(requested));
                }
                emit statusChanged(tr("City lights unavailable — retrying"));
                m_debounce.start(MapProviderRetryPolicy::kConsumerRetryMs);
                return;
            }
            m_loaded = requested;
            m_original = image;
            renderImage();
            emit statusChanged({});
            if (!m_loaded.covers(m_view)) {
                m_debounce.start(0);
            }
        });
        watcher->setFuture(QtConcurrent::run(&CityLightsSource::decode, bytes, requested.size));
    });
}

void CityLightsSource::renderImage()
{
    if (!m_enabled || m_original.isNull()) {
        return;
    }
    if (m_rendering) {
        m_renderAgain = true;
        return;
    }
    m_rendering = true;
    const qint64 key = m_original.cacheKey();
    const bool nightOnly = m_nightOnly;
    const int faintLights = m_faintLights;
    const int warmth = m_warmth;
    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, key, nightOnly, faintLights, warmth] {
        const QImage image = watcher->result();
        watcher->deleteLater();
        m_rendering = false;
        if (m_enabled && key == m_original.cacheKey() && nightOnly == m_nightOnly
            && faintLights == m_faintLights && warmth == m_warmth) {
            m_image = image;
            m_imageBounds = m_loaded.bounds;
            emit imageChanged();
        }
        if (m_renderAgain) {
            m_renderAgain = false;
            renderImage();
        }
    });
    watcher->setFuture(QtConcurrent::run(&CityLightsSource::nightImage, m_original,
        m_loaded.bounds, QDateTime::currentDateTimeUtc(), nightOnly, faintLights, warmth));
}

} // namespace AetherSDR
