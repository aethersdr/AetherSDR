#include "WeatherRadarSource.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QTimeZone>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

namespace AetherSDR {

namespace {
constexpr qint64 kRefreshSeconds = 5 * 60;
constexpr double kWebMercatorExtent = 20037508.342789244;
}

WeatherRadarSource::WeatherRadarSource(Provider provider,
                                       const QDateTime& frameTime,
                                       FrameMode mode,
                                       const QDateTime& sampleTime)
    : m_provider(provider)
    , m_frameMode(mode)
{
    if (mode == FrameMode::Historical) {
        m_frameTime = frameTime.toUTC();
        m_sampleTime = sampleTime.isValid()
            ? sampleTime.toUTC() : m_frameTime;
    } else {
        const qint64 epoch = frameTime.toUTC().toSecsSinceEpoch();
        m_frameTime = QDateTime::fromSecsSinceEpoch(
            epoch - epoch % kRefreshSeconds, QTimeZone::UTC);
        m_sampleTime = m_frameTime;
    }
}

WeatherRadarSource WeatherRadarSource::historicalNoaaFrame(
    const QDateTime& frameTime, const QDateTime& sampleTime,
    const QVector<qint64>& rasterIds)
{
    WeatherRadarSource source(Provider::NoaaMrms, frameTime,
                              FrameMode::Historical, sampleTime);
    source.m_rasterIds = rasterIds;
    std::sort(source.m_rasterIds.begin(), source.m_rasterIds.end());
    source.m_rasterIds.erase(
        std::unique(source.m_rasterIds.begin(), source.m_rasterIds.end()),
        source.m_rasterIds.end());
    return source;
}

QUrl WeatherRadarSource::noaaTimelineUrl()
{
    QUrl url(QStringLiteral(
        "https://mapservices.weather.noaa.gov/eventdriven/rest/services/"
        "radar/radar_base_reflectivity_time/ImageServer/query"));
    QUrlQuery query;
    // Fetch every NOAA coverage record in one bounded catalog response. The
    // parser uses CONUS as the playback clock, then locks the contemporaneous
    // Alaska/Hawaii/Caribbean/Guam raster IDs into the same export. This keeps
    // regional ingest jitter from creating pseudo-frames without sacrificing
    // NOAA's non-CONUS coverage.
    query.addQueryItem(QStringLiteral("where"),
                       QStringLiteral("1=1"));
    query.addQueryItem(QStringLiteral("outFields"),
                       QStringLiteral(
                           "objectid,idp_subset,idp_validtime,"
                           "idp_validendtime"));
    query.addQueryItem(QStringLiteral("returnGeometry"),
                       QStringLiteral("false"));
    query.addQueryItem(QStringLiteral("orderByFields"),
                       QStringLiteral("idp_validtime ASC"));
    query.addQueryItem(QStringLiteral("f"), QStringLiteral("json"));
    url.setQuery(query);
    return url;
}

QUrl WeatherRadarSource::noaaRasterAvailabilityUrl(const QVector<qint64>& rasterIds)
{
    QUrl url = noaaTimelineUrl();
    QStringList ids;
    for (qint64 id : rasterIds) {
        ids.append(QString::number(id));
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("objectIds"), ids.join(','));
    query.addQueryItem(QStringLiteral("returnIdsOnly"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("f"), QStringLiteral("json"));
    url.setQuery(query);
    return url;
}

std::optional<bool> WeatherRadarSource::parseNoaaRasterAvailability(
    const QByteArray& json, const QVector<qint64>& rasterIds)
{
    const QJsonObject object = QJsonDocument::fromJson(json).object();
    if (rasterIds.isEmpty() || object.contains(QStringLiteral("error"))
        || object.value(QStringLiteral("exceededTransferLimit")).toBool()
        || !object.value(QStringLiteral("objectIds")).isArray()) {
        return std::nullopt;
    }
    QSet<qint64> available;
    for (const QJsonValue& value : object.value(QStringLiteral("objectIds")).toArray()) {
        const qint64 id = value.toInteger(-1);
        if (id <= 0) {
            return std::nullopt;
        }
        available.insert(id);
    }
    return std::all_of(rasterIds.cbegin(), rasterIds.cend(),
        [&available](qint64 id) { return available.contains(id); });
}

QVector<WeatherRadarObservation> WeatherRadarSource::parseNoaaTimeline(
    const QByteArray& json, int historyHours)
{
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject() || document.object().contains(QStringLiteral("error"))
        || document.object().value(QStringLiteral("exceededTransferLimit")).toBool()) {
        return {};
    }
    struct CatalogRecord {
        qint64 objectId{0};
        QString subset;
        qint64 validTime{0};
        qint64 validEndTime{0};
    };
    QVector<CatalogRecord> records;
    const QJsonArray features =
        document.object().value(QStringLiteral("features")).toArray();
    records.reserve(features.size());
    for (const QJsonValue& featureValue : features) {
        const QJsonObject attributes = featureValue.toObject()
            .value(QStringLiteral("attributes")).toObject();
        const qint64 objectId = static_cast<qint64>(
            attributes.value(QStringLiteral("objectid")).toDouble());
        const QString subset = attributes
            .value(QStringLiteral("idp_subset")).toString();
        const qint64 validTime = static_cast<qint64>(
            attributes.value(QStringLiteral("idp_validtime")).toDouble());
        const qint64 validEndTime = static_cast<qint64>(
            attributes.value(QStringLiteral("idp_validendtime")).toDouble());
        if (objectId > 0 && !subset.isEmpty() && validTime > 0
            && validEndTime >= validTime) {
            records.append(
                {objectId, subset, validTime, validEndTime});
        }
    }
    QVector<CatalogRecord> conusRecords;
    QHash<QString, qint64> newestBySubset;
    for (const CatalogRecord& record : std::as_const(records)) {
        newestBySubset[record.subset] = std::max(
            newestBySubset.value(record.subset), record.validTime);
    }
    for (const CatalogRecord& record : std::as_const(records)) {
        // NOAA's newest ingested raster has start == end until the following
        // observation arrives. That means an OPEN interval, not an unusable
        // image. We lock its exact raster ID, so no later scan is required.
        if (record.subset == QStringLiteral("CONUS")
            && (record.validEndTime > record.validTime
                || record.validTime == newestBySubset.value(record.subset))) {
            conusRecords.append(record);
        }
    }
    if (conusRecords.isEmpty()) {
        return {};
    }
    std::sort(records.begin(), records.end(),
              [](const auto& left, const auto& right) {
                  if (left.validTime != right.validTime) {
                      return left.validTime < right.validTime;
                  }
                  return left.objectId < right.objectId;
              });
    std::sort(conusRecords.begin(), conusRecords.end(),
              [](const auto& left, const auto& right) {
                  if (left.validTime != right.validTime) {
                      return left.validTime < right.validTime;
                  }
                  return left.objectId < right.objectId;
              });
    conusRecords.erase(std::unique(
        conusRecords.begin(), conusRecords.end(),
        [](const auto& left, const auto& right) {
            return left.validTime == right.validTime;
        }), conusRecords.end());

    const int boundedHours = std::clamp(historyHours, 1, 4);
    const qint64 earliest = conusRecords.last().validTime
        - static_cast<qint64>(boundedHours) * 60 * 60 * 1000;
    QVector<WeatherRadarObservation> result;
    for (const CatalogRecord& conus : std::as_const(conusRecords)) {
        if (conus.validTime < earliest) {
            continue;
        }
        const qint64 sampleTime = conus.validTime
            + (conus.validEndTime - conus.validTime) / 2;
        QHash<QString, CatalogRecord> selectedBySubset;
        for (const CatalogRecord& record : std::as_const(records)) {
            const bool open = record.validEndTime == record.validTime;
            if (record.validTime > sampleTime
                || (open && record.validTime != newestBySubset.value(record.subset))
                || (!open && record.validEndTime <= sampleTime)) {
                continue;
            }
            const auto existing = selectedBySubset.constFind(record.subset);
            if (existing == selectedBySubset.cend()
                || existing->validTime < record.validTime) {
                selectedBySubset.insert(record.subset, record);
            }
        }
        QVector<qint64> rasterIds;
        rasterIds.reserve(selectedBySubset.size());
        for (const CatalogRecord& selected
             : std::as_const(selectedBySubset)) {
            rasterIds.append(selected.objectId);
        }
        std::sort(rasterIds.begin(), rasterIds.end());
        if (rasterIds.isEmpty()) {
            continue;
        }
        result.append({
            QDateTime::fromMSecsSinceEpoch(
                conus.validTime, QTimeZone::UTC),
            QDateTime::fromMSecsSinceEpoch(
                sampleTime, QTimeZone::UTC),
            std::move(rasterIds)});
    }
    return result;
}

WeatherRadarSource WeatherRadarSource::currentNoaaFrame()
{
    return WeatherRadarSource(Provider::NoaaMrms,
                              QDateTime::currentDateTimeUtc());
}

QRectF WeatherRadarSource::conventionalBoundsFromQgv(
    const QRectF& qgvBounds)
{
    const QRectF normalized = qgvBounds.normalized();
    return QRectF(normalized.left(), -normalized.bottom(),
                  normalized.width(), normalized.height()).normalized();
}

QString WeatherRadarSource::frameId() const
{
    QStringList rasterIdStrings;
    rasterIdStrings.reserve(m_rasterIds.size());
    for (const qint64 rasterId : m_rasterIds) {
        rasterIdStrings.append(QString::number(rasterId));
    }
    return QStringLiteral("noaa-mrms-%1-%2-%3-%4")
        .arg(m_frameMode == FrameMode::Historical
                 ? QStringLiteral("history") : QStringLiteral("live"))
        .arg(m_frameTime.toMSecsSinceEpoch())
        .arg(m_sampleTime.toMSecsSinceEpoch())
        .arg(rasterIdStrings.join(QLatin1Char('-')));
}

QString WeatherRadarSource::attribution() const
{
    return QStringLiteral("Radar: NOAA/NWS");
}

QUrl WeatherRadarSource::tileUrl(int zoom, int x, int y) const
{
    if (m_provider != Provider::NoaaMrms || zoom < minimumZoom()
        || zoom > maximumZoom()) {
        return {};
    }
    const int tileCount = 1 << zoom;
    if (y < 0 || y >= tileCount) {
        return {};
    }
    const int wrappedX = ((x % tileCount) + tileCount) % tileCount;
    const double tileSpan = 2.0 * kWebMercatorExtent / tileCount;
    const double minimumX = -kWebMercatorExtent + wrappedX * tileSpan;
    const double maximumX = minimumX + tileSpan;
    const double maximumY = kWebMercatorExtent - y * tileSpan;
    const double minimumY = maximumY - tileSpan;

    return imageUrl(QRectF(QPointF(minimumX, minimumY),
                           QPointF(maximumX, maximumY)),
                    QSize(256, 256));
}

QUrl WeatherRadarSource::imageUrl(
    const QRectF& webMercatorBounds, const QSize& pixelSize) const
{
    const QRectF bounds = webMercatorBounds.normalized();
    if (!bounds.isValid() || bounds.isEmpty()
        || pixelSize.width() <= 0 || pixelSize.height() <= 0
        || pixelSize.width() > 4096 || pixelSize.height() > 4096) {
        return {};
    }
    const bool historical = m_frameMode == FrameMode::Historical;
    QUrl url(historical
        ? QStringLiteral(
            "https://mapservices.weather.noaa.gov/eventdriven/rest/services/"
            "radar/radar_base_reflectivity_time/ImageServer/exportImage")
        : QStringLiteral(
            "https://mapservices.weather.noaa.gov/eventdriven/rest/services/"
            "radar/radar_base_reflectivity/MapServer/export"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("bbox"),
        QStringLiteral("%1,%2,%3,%4")
            .arg(bounds.left(), 0, 'f', 3)
            .arg(bounds.top(), 0, 'f', 3)
            .arg(bounds.right(), 0, 'f', 3)
            .arg(bounds.bottom(), 0, 'f', 3));
    query.addQueryItem(QStringLiteral("bboxSR"), QStringLiteral("3857"));
    query.addQueryItem(QStringLiteral("imageSR"), QStringLiteral("3857"));
    query.addQueryItem(QStringLiteral("size"),
        QStringLiteral("%1,%2").arg(pixelSize.width())
                                  .arg(pixelSize.height()));
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("png32"));
    query.addQueryItem(QStringLiteral("transparent"), QStringLiteral("true"));
    QByteArray encodedMosaicRule;
    if (historical) {
        // We place raw image bytes in precisely this bbox. ArcGIS otherwise
        // changes the extent to fit the requested size (not reported by
        // f=image), which misregisters capped/wrapped/rounded exports.
        query.addQueryItem(QStringLiteral("adjustAspectRatio"), QStringLiteral("false"));
        if (!m_rasterIds.isEmpty()) {
            QJsonArray lockedIds;
            for (const qint64 rasterId : m_rasterIds) {
                lockedIds.append(rasterId);
            }
            QJsonObject mosaicRule;
            mosaicRule.insert(
                QStringLiteral("mosaicMethod"),
                QStringLiteral("esriMosaicLockRaster"));
            mosaicRule.insert(QStringLiteral("lockRasterIds"), lockedIds);
            mosaicRule.insert(
                QStringLiteral("mosaicOperation"),
                QStringLiteral("MT_FIRST"));
            encodedMosaicRule = QUrl::toPercentEncoding(
                QString::fromUtf8(QJsonDocument(mosaicRule)
                                      .toJson(QJsonDocument::Compact)));
        } else {
            query.addQueryItem(
                QStringLiteral("time"),
                QString::number(m_sampleTime.toMSecsSinceEpoch()));
        }
    } else {
        query.addQueryItem(QStringLiteral("layers"), QStringLiteral("show:3"));
    }
    query.addQueryItem(QStringLiteral("f"), QStringLiteral("image"));
    // The service returns its current mosaic. The bucket changes the cache key
    // at the documented refresh cadence without implying an exact scan time.
    if (!historical) {
        query.addQueryItem(QStringLiteral("refresh"),
                           QString::number(m_frameTime.toSecsSinceEpoch()));
    }
    url.setQuery(query);
    if (!encodedMosaicRule.isEmpty()) {
        // QUrlQuery intentionally leaves some reserved JSON characters such
        // as '[' and ']' readable. ArcGIS rejects that otherwise-valid URL,
        // so append this one opaque value in its explicitly percent-encoded
        // form and construct the final QUrl from encoded bytes.
        QByteArray encodedUrl = url.toEncoded();
        encodedUrl.append('&');
        encodedUrl.append("mosaicRule=");
        encodedUrl.append(encodedMosaicRule);
        url = QUrl::fromEncoded(encodedUrl, QUrl::StrictMode);
    }
    return url;
}

} // namespace AetherSDR
