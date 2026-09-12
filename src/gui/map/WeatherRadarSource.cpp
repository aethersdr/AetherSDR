#include "WeatherRadarSource.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QTimeZone>
#include <QUrlQuery>
#include <QXmlStreamReader>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
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

WeatherRadarSource WeatherRadarSource::latestFrame() const
{
    WeatherRadarSource source(m_provider);
    source.m_enabledProviders = m_enabledProviders;
    return source;
}

WeatherRadarSource WeatherRadarSource::historicalFrame(const QDateTime& time,
    const QDateTime& sample, const QVector<qint64>& ids) const
{
    if (m_provider == Provider::NoaaMrms) {
        return historicalNoaaFrame(time, sample, ids);
    }
    WeatherRadarSource source(m_provider, time, FrameMode::Historical, sample);
    source.m_enabledProviders = m_enabledProviders;
    return source;
}

WeatherRadarSource WeatherRadarSource::composite(int enabledProviders)
{
    WeatherRadarSource source(Provider::Composite);
    source.m_enabledProviders = enabledProviders & 15;
    return source;
}

QUrl WeatherRadarSource::operaTimelineUrl()
{
    return QUrl(QStringLiteral("opera-radar://timeline"));
}

QUrl WeatherRadarSource::operaFrameUrl(const QDateTime& time)
{
    return QUrl(QStringLiteral("https://s3.waw3-1.cloudferro.com/openradar-24h/%1/OPERA/COMP/OPERA@%2@0@DBZH.tiff")
        .arg(time.toUTC().toString(QStringLiteral("yyyy/MM/dd")),
             time.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmm"))));
}

QString WeatherRadarSource::productDescription() const
{
    if (m_provider == Provider::Composite) {
        return (m_enabledProviders & 8)
            ? QStringLiteral("LibreWXR: radar + satellite/model estimates. Regional feeds are backups. Global frames: up to 2 h history.")
            : QStringLiteral("US / Europe: reflectivity (dBZ). Canada: rain rate (mm/h). Coverage depends on available regional feeds.");
    }
    if (m_provider == Provider::Opera) {
        return QStringLiteral("EUMETNET OPERA · Europe · maximum reflectivity (dBZ) · CC BY 4.0");
    }
    return m_provider == Provider::Eccc
        ? QStringLiteral("ECCC · North America · rainfall rate (mm/h)")
        : QStringLiteral("NOAA/NWS · US regions · base reflectivity (dBZ)");
}

WeatherRadarSource WeatherRadarSource::playbackSourceForTimeline(const QByteArray& bytes) const
{
    if (m_provider != Provider::Composite) {
        return *this;
    }
    const int providers = QJsonDocument::fromJson(bytes).object()
        .value(QStringLiteral("providers")).toInt(0);
    // Pin one coverage set for the movie. A transient primary tile failure
    // must not turn one frame into a regional-only "successful" replacement.
    const bool allowed = providers != 0 && (providers & ~m_enabledProviders) == 0
        && (providers == 8 || (providers & 8) == 0);
    return composite(allowed ? providers
        : (m_enabledProviders & 8) ? 8 : m_enabledProviders);
}

QUrl WeatherRadarSource::timelineUrl() const
{
    if (m_provider == Provider::Composite) { return QUrl(QStringLiteral("radar-composite://timeline?providers=%1").arg(m_enabledProviders)); }
    if (m_provider == Provider::Opera) { return operaTimelineUrl(); }
    if (m_provider == Provider::LibreWxr) { return QUrl(QStringLiteral("https://api.librewxr.net/public/weather-maps.json")); }
    if (m_provider == Provider::NoaaMrms) {
        return noaaTimelineUrl();
    }
    return QUrl(QStringLiteral("https://geo.weather.gc.ca/geomet?service=WMS&version=1.3.0&request=GetCapabilities&layer=RADAR_1KM_RRAI"));
}

QVector<WeatherRadarObservation> WeatherRadarSource::parseTimeline(
    const QByteArray& bytes, int hours) const
{
    if (m_provider == Provider::NoaaMrms) {
        return parseNoaaTimeline(bytes, hours);
    }
    if (bytes.size() > 512 * 1024) {
        return {};
    }
    if (m_provider == Provider::LibreWxr) {
        const QJsonObject root = QJsonDocument::fromJson(bytes).object();
        if (root.value(QStringLiteral("host")).toString() != QStringLiteral("https://api.librewxr.net")) { return {}; }
        const QJsonArray past = root.value(QStringLiteral("radar")).toObject().value(QStringLiteral("past")).toArray();
        if (past.isEmpty() || past.size() > 60) { return {}; }
        QVector<WeatherRadarObservation> frames;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        qint64 previous = 0;
        for (const auto& value : past) {
            const QJsonObject frame = value.toObject();
            const auto rawTime = frame.value(QStringLiteral("time"));
            const double epoch = rawTime.toDouble(-1);
            if (!rawTime.isDouble() || !std::isfinite(epoch) || epoch != std::floor(epoch)
                || epoch < now - 5 * 3600 || epoch > now || epoch <= previous) { return {}; }
            const qint64 stamp = qint64(epoch);
            if (frame.value(QStringLiteral("path")).toString() != QStringLiteral("/v2/radar/%1").arg(stamp)) { return {}; }
            previous = stamp;
            const QDateTime time = QDateTime::fromSecsSinceEpoch(stamp, QTimeZone::UTC);
            frames.append({time, time, {}});
        }
        // Only published past frames. The nowcast array is deliberately ignored.
        if (now - previous > 1800) { return {}; }
        const QDateTime first = frames.last().frameTime.addSecs(-std::clamp(hours, 1, 4) * 3600);
        frames.erase(std::remove_if(frames.begin(), frames.end(), [&first](const auto& f) { return f.frameTime < first; }), frames.end());
        return frames;
    }
    if (m_provider == Provider::Composite) {
        QVector<WeatherRadarObservation> frames;
        const QJsonArray times = QJsonDocument::fromJson(bytes).object().value(QStringLiteral("times")).toArray();
        if (times.size() > 60) { return {}; }
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QDateTime previous;
        for (const auto& value : times) {
            if (!value.isDouble()) { return {}; }
            const double epoch = value.toDouble();
            if (!std::isfinite(epoch) || epoch < now.addSecs(-5 * 3600).toSecsSinceEpoch()
                || epoch > now.toSecsSinceEpoch()) { return {}; }
            const QDateTime time = QDateTime::fromSecsSinceEpoch(qint64(epoch), QTimeZone::UTC);
            if (previous.isValid() && time <= previous) { return {}; }
            previous = time;
            frames.append({time, time, {}});
        }
        if (!frames.isEmpty()) {
            const QDateTime first = frames.last().frameTime.addSecs(-std::clamp(hours, 1, 4) * 3600);
            frames.erase(std::remove_if(frames.begin(), frames.end(), [&first](const auto& frame) { return frame.frameTime < first; }), frames.end());
        }
        return frames;
    }
    if (m_provider == Provider::Opera) {
        const QJsonArray links = QJsonDocument::fromJson(bytes).object().value(QStringLiteral("links")).toArray();
        if (links.size() > 200) { return {}; }
        QVector<WeatherRadarObservation> frames;
        QSet<QDateTime> seen;
        const QRegularExpression pattern(QStringLiteral("OPERA@([0-9]{8}T[0-9]{4})@0@DBZH\\.tiff$"));
        const QDateTime now = QDateTime::currentDateTimeUtc();
        for (const QJsonValue& entry : links) {
            const QUrl url(entry.toObject().value(QStringLiteral("href")).toString());
            const auto match = pattern.match(url.path());
            if (!match.hasMatch()) { continue; }
            QDateTime time = QDateTime::fromString(match.captured(1), QStringLiteral("yyyyMMdd'T'HHmm"));
            time.setTimeZone(QTimeZone::UTC);
            if (!time.isValid() || url != operaFrameUrl(time) || time > now.addSecs(600)
                || time < now.addSecs(-5 * 3600) || seen.contains(time)) { continue; }
            seen.insert(time); frames.append({time, time, {}});
        }
        std::sort(frames.begin(), frames.end(), [](const auto& a, const auto& b) { return a.frameTime < b.frameTime; });
        if (!frames.isEmpty()) {
            const QDateTime first = frames.last().frameTime.addSecs(-std::clamp(hours, 1, 4) * 3600);
            frames.erase(std::remove_if(frames.begin(), frames.end(), [&first](const auto& f) { return f.frameTime < first; }), frames.end());
        }
        return frames;
    }
    // Only the requested WMS layer's time dimension is authoritative.
    QXmlStreamReader xml(bytes);
    QVector<QPair<int, QString>> layers;
    int depth = 0;
    bool foundTime = false;
    QVector<WeatherRadarObservation> result;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement()) {
            if (!layers.isEmpty() && layers.last().first == depth) { layers.removeLast(); }
            --depth;
            continue;
        }
        if (!xml.isStartElement()) { continue; }
        ++depth;
        if (xml.name() == QStringLiteral("Layer")) {
            layers.append(qMakePair(depth, QString{}));
        } else if (!layers.isEmpty() && depth == layers.last().first + 1
            && xml.name() == QStringLiteral("Name")) {
            layers.last().second = xml.readElementText();
            --depth;
        } else if (!layers.isEmpty() && depth == layers.last().first + 1
                   && xml.name() == QStringLiteral("Dimension")
                   && layers.last().second == QStringLiteral("RADAR_1KM_RRAI")
                   && xml.attributes().value(QStringLiteral("name")) == QStringLiteral("time")) {
            if (foundTime) { return {}; }
            foundTime = true;
            const QStringList interval = xml.readElementText().split('/');
            --depth;
            if (interval.size() != 3) {
                return {};
            }
            const QDateTime first = QDateTime::fromString(interval[0], Qt::ISODate).toUTC();
            const QDateTime last = QDateTime::fromString(interval[1], Qt::ISODate).toUTC();
            const QRegularExpressionMatch step = QRegularExpression(
                QStringLiteral("^PT([1-9][0-9]?)M$")).match(interval[2]);
            if (!first.isValid() || !last.isValid() || !step.hasMatch()
                || first > last || first.secsTo(last) > 24 * 3600
                || last < QDateTime::currentDateTimeUtc().addSecs(-5 * 3600)
                || last > QDateTime::currentDateTimeUtc().addSecs(600)) {
                return {};
            }
            const QDateTime earliest = last.addSecs(-std::clamp(hours, 1, 3) * 3600);
            for (QDateTime time = first; time <= last; time = time.addSecs(step.captured(1).toInt() * 60)) {
                if (time >= earliest) {
                    result.append({time, time, {}});
                }
            }
        }
    }
    return xml.hasError() ? QVector<WeatherRadarObservation>{} : result;
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
    return QStringLiteral("%5-%1-%2-%3-%4")
        .arg(m_frameMode == FrameMode::Historical
                 ? QStringLiteral("history") : QStringLiteral("live"))
        .arg(m_frameTime.toMSecsSinceEpoch())
        .arg(m_sampleTime.toMSecsSinceEpoch())
        .arg(rasterIdStrings.join(QLatin1Char('-')))
        .arg(m_provider == Provider::Composite ? QStringLiteral("radar-composite-%1").arg(m_enabledProviders) : m_provider == Provider::LibreWxr ? QStringLiteral("librewxr-mixed") : m_provider == Provider::Opera ? QStringLiteral("opera-dbzh") : m_provider == Provider::Eccc ? QStringLiteral("eccc-rrai") : QStringLiteral("noaa-mrms"));
}

QString WeatherRadarSource::attribution() const
{
    if (m_provider == Provider::Composite) {
        QStringList names;
        if (m_enabledProviders & 8) { names << QStringLiteral("LibreWXR · radar/satellite/model · CC BY / BY-SA 4.0"); }
        if (m_enabledProviders & 1) { names << QStringLiteral("NOAA/NWS (dBZ)"); }
        if (m_enabledProviders & 2) { names << QStringLiteral("ECCC/MSC (mm/h)"); }
        if (m_enabledProviders & 4) { names << QStringLiteral("EUMETNET OPERA (dBZ, CC BY 4.0)"); }
        return QStringLiteral("Radar: ") + names.join(QStringLiteral(" · "));
    }
    if (m_provider == Provider::Opera) { return QStringLiteral("Radar: EUMETNET OPERA · CC BY 4.0 · max reflectivity (dBZ)"); }
    return m_provider == Provider::Eccc ? QStringLiteral("Radar: ECCC / MSC · rain rate (mm/h)")
        : QStringLiteral("Radar: NOAA/NWS · reflectivity (dBZ)");
}

int WeatherRadarSource::tilePixelSize() const
{
    // All adapters support Retina-sized tiles, including direct regional feeds.
    return 512;
}

QUrl WeatherRadarSource::tileUrl(int zoom, int x, int y) const
{
    if (zoom < minimumZoom()
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
                    QSize(tilePixelSize(), tilePixelSize()));
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
    if (m_provider == Provider::Opera || m_provider == Provider::Composite || m_provider == Provider::LibreWxr) {
        QUrl url(m_provider == Provider::LibreWxr ? QStringLiteral("libre-radar://image")
            : m_provider == Provider::Opera ? QStringLiteral("opera-radar://image") : QStringLiteral("radar-composite://image"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("time"), m_frameMode == FrameMode::Latest
            ? QStringLiteral("latest") : QString::number(m_frameTime.toSecsSinceEpoch()));
        query.addQueryItem(QStringLiteral("bbox"), QStringLiteral("%1,%2,%3,%4")
            .arg(bounds.left(), 0, 'f', 3).arg(bounds.top(), 0, 'f', 3)
            .arg(bounds.right(), 0, 'f', 3).arg(bounds.bottom(), 0, 'f', 3));
        query.addQueryItem(QStringLiteral("providers"), QString::number(m_enabledProviders));
        query.addQueryItem(QStringLiteral("refresh"), QString::number(m_frameTime.toSecsSinceEpoch()));
        query.addQueryItem(QStringLiteral("width"), QString::number(pixelSize.width()));
        query.addQueryItem(QStringLiteral("height"), QString::number(pixelSize.height()));
        url.setQuery(query);
        return url;
    }
    if (m_provider == Provider::Eccc) {
        QUrl url(QStringLiteral("https://geo.weather.gc.ca/geomet"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("SERVICE"), QStringLiteral("WMS"));
        query.addQueryItem(QStringLiteral("VERSION"), QStringLiteral("1.3.0"));
        query.addQueryItem(QStringLiteral("REQUEST"), QStringLiteral("GetMap"));
        query.addQueryItem(QStringLiteral("LAYERS"), QStringLiteral("RADAR_1KM_RRAI"));
        query.addQueryItem(QStringLiteral("STYLES"), QString{});
        query.addQueryItem(QStringLiteral("CRS"), QStringLiteral("EPSG:3857"));
        query.addQueryItem(QStringLiteral("BBOX"), QStringLiteral("%1,%2,%3,%4")
            .arg(bounds.left(), 0, 'f', 3).arg(bounds.top(), 0, 'f', 3)
            .arg(bounds.right(), 0, 'f', 3).arg(bounds.bottom(), 0, 'f', 3));
        query.addQueryItem(QStringLiteral("WIDTH"), QString::number(pixelSize.width()));
        query.addQueryItem(QStringLiteral("HEIGHT"), QString::number(pixelSize.height()));
        query.addQueryItem(QStringLiteral("FORMAT"), QStringLiteral("image/png"));
        query.addQueryItem(QStringLiteral("TRANSPARENT"), QStringLiteral("TRUE"));
        query.addQueryItem(m_frameMode == FrameMode::Historical
            ? QStringLiteral("TIME") : QStringLiteral("refresh"),
            m_frameMode == FrameMode::Historical ? m_frameTime.toString(Qt::ISODate)
                : QString::number(m_frameTime.toSecsSinceEpoch()));
        url.setQuery(query);
        return url;
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
