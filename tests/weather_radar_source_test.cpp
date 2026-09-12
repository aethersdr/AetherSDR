#include "gui/map/WeatherRadarSource.h"
#include "gui/map/WeatherRadarPlaybackTimeline.h"
#include "gui/map/WeatherRadarWorldWrap.h"
#include "gui/map/WeatherRadarFrameTime.h"
#include "gui/map/WeatherRadarLoadingStatus.h"

using AetherSDR::WeatherRadarObservation;
using AetherSDR::kRadarWorldWidth;
using AetherSDR::weatherRadarCanonicalPlaybackBounds;
using AetherSDR::weatherRadarVisibleWorldOffsets;

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <QUrlQuery>

#include <iostream>
#include <optional>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

} // namespace

int main()
{
    using AetherSDR::WeatherRadarSource;

    const QDateTime input = QDateTime::fromString(
        QStringLiteral("2026-08-30T12:07:43Z"), Qt::ISODate);
    const WeatherRadarSource source(
        WeatherRadarSource::Provider::NoaaMrms, input);
    bool ok = true;
    const auto liveLabel = AetherSDR::weatherRadarFramePresentation(input, true, input.addSecs(3600));
    ok &= expect(liveLabel.text == QStringLiteral("Age unknown")
                     && liveLabel.tooltip.contains(QStringLiteral("observation time is unavailable")),
                 "a live cache bucket must never be labelled as a fresh observation");
    const auto historyLabel = AetherSDR::weatherRadarFramePresentation(input, false, input.addSecs(3600));
    ok &= expect(historyLabel.text.contains(QStringLiteral("60m old")),
                 "historical observation age remains available");
    using Loading = AetherSDR::WeatherRadarLoadingStatus;
    Loading liveStatus;
    ok &= expect(liveStatus.update(0, false, true, 0, 0, true) == Loading::State::Failed
                     && liveStatus.update(60000, false, true, 0, 0, true) == Loading::State::Failed
                     && liveStatus.update(61000, true, false, 4, 0, true) == Loading::State::Failed,
                 "live failure must remain visible during failure and replacement loading");
    ok &= expect(liveStatus.update(62000, false, false, 0, 0, true) == Loading::State::Hidden,
                 "successful live recovery clears the failure notice");
    Loading playbackStatus;
    playbackStatus.update(0, false, true, 0, 0);
    ok &= expect(playbackStatus.update(60000, false, true, 0, 0) == Loading::State::Hidden,
                 "historical imagery keeps its existing brief notice and explicit frame age");
    const QLocale usLocale(QLocale::English, QLocale::UnitedStates);
    const QTimeZone newYork(QByteArrayLiteral("America/New_York"));
    const auto evening = AetherSDR::weatherRadarLocalFrameTime(
        QDateTime::fromString(QStringLiteral("2026-09-07T01:02:15Z"), Qt::ISODate),
        usLocale, newYork);
    // CLDR may separate AM/PM with a narrow no-break space; either spacing
    // is legitimate locale formatting, while the clock/date must be exact.
    ok &= expect(evening.clock.simplified() == QStringLiteral("9:02 PM")
                     && evening.details == QStringLiteral("2026-09-06 21:02:15 EDT"),
                 "radar label must use local clock and the correct prior calendar date");
    const auto beforeDst = AetherSDR::weatherRadarLocalFrameTime(
        QDateTime::fromString(QStringLiteral("2026-11-01T05:30:00Z"), Qt::ISODate),
        usLocale, newYork);
    const auto afterDst = AetherSDR::weatherRadarLocalFrameTime(
        QDateTime::fromString(QStringLiteral("2026-11-01T06:30:00Z"), Qt::ISODate),
        usLocale, newYork);
    ok &= expect(beforeDst.clock == afterDst.clock
                     && beforeDst.details.endsWith(QStringLiteral("EDT"))
                     && afterDst.details.endsWith(QStringLiteral("EST")),
                 "repeated DST hours must retain their actual observation timezone in the tooltip");
    ok &= expect(source.frameTime() == QDateTime::fromString(
        QStringLiteral("2026-08-30T12:05:00Z"), Qt::ISODate),
        "NOAA refresh key should use a five-minute UTC bucket");
    ok &= expect(source.tileUrl(-1, 0, 0).isEmpty(),
                 "negative zoom must be rejected");
    ok &= expect(source.tileUrl(13, 0, 0).isEmpty(),
                 "zoom above the provider limit must be rejected");
    ok &= expect(source.tileUrl(2, 0, -1).isEmpty(),
                 "tile y above the Web Mercator world must be rejected");
    ok &= expect(source.tileUrl(2, 0, 4).isEmpty(),
                 "tile y below the Web Mercator world must be rejected");

    const QUrl base = source.tileUrl(2, 0, 1);
    const QUrl wrapped = source.tileUrl(2, 4, 1);
    ok &= expect(base.scheme() == QStringLiteral("https"),
                 "radar endpoint must use HTTPS");
    ok &= expect(base.host()
                     == QStringLiteral("mapservices.weather.noaa.gov"),
                 "radar endpoint must remain on the NOAA host");
    ok &= expect(QUrlQuery(base).queryItemValue(QStringLiteral("bbox"))
                     == QUrlQuery(wrapped).queryItemValue(
                         QStringLiteral("bbox")),
                 "horizontal world copies must share a canonical tile");

    const QUrlQuery query(base);
    ok &= expect(query.queryItemValue(QStringLiteral("bboxSR"))
                     == QStringLiteral("3857"),
                 "tile bbox must declare Web Mercator");
    ok &= expect(query.queryItemValue(QStringLiteral("imageSR"))
                     == QStringLiteral("3857"),
                 "tile image must request Web Mercator");
    ok &= expect(query.queryItemValue(QStringLiteral("size"))
                     == QStringLiteral("512,512"),
                 "radar tiles must retain 512 pixels for Retina overviews");
    ok &= expect(query.queryItemValue(QStringLiteral("transparent"))
                     == QStringLiteral("true"),
                 "radar export must preserve transparent no-echo areas");
    ok &= expect(query.queryItemValue(QStringLiteral("layers"))
                     == QStringLiteral("show:3"),
                 "radar export must select the composite image layer");
    ok &= expect(query.queryItemValue(QStringLiteral("refresh"))
                     == QString::number(source.frameTime().toSecsSinceEpoch()),
                 "cache key must identify the current refresh bucket");

    const QDateTime historicalTime = QDateTime::fromMSecsSinceEpoch(
        1788149097000LL, QTimeZone::UTC);
    const WeatherRadarSource historical =
        WeatherRadarSource::historicalNoaaFrame(historicalTime);
    const QUrl historicalUrl = historical.tileUrl(2, 0, 1);
    const QUrlQuery historicalQuery(historicalUrl);
    ok &= expect(historicalUrl.path().endsWith(
                     QStringLiteral("/ImageServer/exportImage")),
                 "historical frames must use NOAA's time-enabled image service");
    ok &= expect(historicalQuery.queryItemValue(QStringLiteral("time"))
                     == QStringLiteral("1788149097000"),
                 "historical export must select one exact timeline instant");
    ok &= expect(!historicalQuery.hasQueryItem(QStringLiteral("layers")),
                 "image-service requests must not use MapServer layer syntax");
    ok &= expect(!historicalQuery.hasQueryItem(QStringLiteral("refresh")),
                 "historical frame URLs must remain immutable cache keys");

    const QUrl compositeUrl = historical.imageUrl(
        QRectF(-1000.0, -500.0, 2000.0, 1000.0), QSize(1200, 700));
    const QUrlQuery compositeQuery(compositeUrl);
    ok &= expect(compositeQuery.queryItemValue(QStringLiteral("bbox"))
                     == QStringLiteral("-1000.000,-500.000,1000.000,500.000"),
                 "buffered frames must preserve the requested map extent");
    ok &= expect(compositeQuery.queryItemValue(QStringLiteral("size"))
                     == QStringLiteral("1200,700"),
                 "buffered frames must request the viewport resolution");
    ok &= expect(historical.imageUrl(
                     QRectF(-1.0, -1.0, 2.0, 2.0), QSize(5000, 5000))
                     .isEmpty(),
                 "buffered frame dimensions must remain bounded");

    const QRectF qgvCameraBounds(-11000000.0, -6500000.0,
                                 5000000.0, 4000000.0);
    const QRectF conventionalBounds =
        WeatherRadarSource::conventionalBoundsFromQgv(qgvCameraBounds);
    ok &= expect(conventionalBounds
                     == QRectF(-11000000.0, 2500000.0,
                               5000000.0, 4000000.0),
                 "QGeoView camera Y must be inverted for the NOAA bbox");
    const QUrl convertedUrl = historical.imageUrl(
        conventionalBounds, QSize(1200, 700));
    ok &= expect(QUrlQuery(convertedUrl).queryItemValue(
                     QStringLiteral("bbox"))
                     == QStringLiteral(
                         "-11000000.000,2500000.000,-6000000.000,6500000.000"),
                 "flat playback must request the northern camera extent");

    const QUrlQuery timelineQuery(WeatherRadarSource::noaaTimelineUrl());
    ok &= expect(timelineQuery.queryItemValue(QStringLiteral("where"))
                     == QStringLiteral("1=1"),
                 "timeline must fetch every NOAA radar coverage region");
    ok &= expect(timelineQuery.queryItemValue(QStringLiteral("outFields"))
                     == QStringLiteral(
                         "objectid,idp_subset,idp_validtime,"
                         "idp_validendtime"),
                 "timeline must request raster identity and validity intervals");

    const QDateTime safeSampleTime = historicalTime.addSecs(180);
    const WeatherRadarSource safelySampled =
        WeatherRadarSource::historicalNoaaFrame(
            historicalTime, safeSampleTime);
    ok &= expect(safelySampled.frameTime() == historicalTime,
                 "safe sampling must preserve the displayed observation time");
    ok &= expect(QUrlQuery(safelySampled.imageUrl(
                     QRectF(-1000.0, -500.0, 2000.0, 1000.0),
                     QSize(1200, 700)))
                     .queryItemValue(QStringLiteral("time"))
                     == QString::number(
                         safeSampleTime.toMSecsSinceEpoch()),
                 "historical export must sample inside the finalized interval");

    const WeatherRadarSource lockedRaster =
        WeatherRadarSource::historicalNoaaFrame(
            historicalTime, safeSampleTime, {42, 7, 42});
    const QUrlQuery lockedQuery(lockedRaster.imageUrl(
        QRectF(-1000.0, -500.0, 2000.0, 1000.0), QSize(1200, 700)));
    const QByteArray lockedEncodedUrl = lockedRaster.imageUrl(
        QRectF(-1000.0, -500.0, 2000.0, 1000.0),
        QSize(1200, 700)).toEncoded();
    ok &= expect(!lockedQuery.hasQueryItem(QStringLiteral("time")),
                 "an explicit raster lock must remove ambiguous time selection");
    ok &= expect(lockedEncodedUrl.contains("%5B7%2C42%5D")
                     && !lockedEncodedUrl.contains("[7,42]"),
                 "raster-lock JSON must be fully encoded for ArcGIS");
    const QJsonDocument lockedRule = QJsonDocument::fromJson(
        QUrl::fromPercentEncoding(lockedQuery.queryItemValue(
            QStringLiteral("mosaicRule")).toUtf8()).toUtf8());
    ok &= expect(lockedRule.object()
                     .value(QStringLiteral("mosaicMethod")).toString()
                     == QStringLiteral("esriMosaicLockRaster"),
                 "historical exports must lock NOAA's exact raster records");
    ok &= expect(lockedRule.object()
                     .value(QStringLiteral("lockRasterIds")).toArray()
                     == QJsonArray({7, 42}),
                 "raster locks must be stable, sorted, and deduplicated");

    const QByteArray timelineJson = R"({"features":[
        {"attributes":{"objectid":1,"idp_subset":"CONUS","idp_validtime":1700000000000,"idp_validendtime":1700000600000}},
        {"attributes":{"objectid":2,"idp_subset":"CONUS","idp_validtime":1700001240000,"idp_validendtime":1700001840000}},
        {"attributes":{"objectid":3,"idp_subset":"CONUS","idp_validtime":1700001240000,"idp_validendtime":1700001840000}},
        {"attributes":{"objectid":10,"idp_subset":"HAWAII","idp_validtime":1700001200000,"idp_validendtime":1700001900000}},
        {"attributes":{"objectid":4,"idp_subset":"CONUS","idp_validtime":1700004240000,"idp_validendtime":1700004840000}},
        {"attributes":{"objectid":11,"idp_subset":"HAWAII","idp_validtime":1700004200000,"idp_validendtime":1700004900000}},
        {"attributes":{"objectid":5,"idp_subset":"CONUS","idp_validtime":1700004840000,"idp_validendtime":1700004840000}}
    ]})";
    const QVector<AetherSDR::WeatherRadarObservation> oneHourFrames =
        WeatherRadarSource::parseNoaaTimeline(timelineJson, 1);
    QJsonObject truncatedCatalog = QJsonDocument::fromJson(timelineJson).object();
    truncatedCatalog.insert(QStringLiteral("exceededTransferLimit"), true);
    ok &= expect(WeatherRadarSource::parseNoaaTimeline(
        QJsonDocument(truncatedCatalog).toJson(), 1).isEmpty(),
        "a truncated catalog must not retire observations missing from that page");
    ok &= expect(WeatherRadarSource::parseNoaaRasterAvailability(
        R"({"objectIds":[7,42]})", {42,7}) == std::optional<bool>(true),
        "all locked rasters must still exist before accepting clear weather");
    ok &= expect(WeatherRadarSource::parseNoaaRasterAvailability(
        R"({"objectIds":[42]})", {7,42}) == std::optional<bool>(false),
        "even a partially expired regional mosaic must not replace an original");
    ok &= expect(WeatherRadarSource::parseNoaaRasterAvailability(
        R"({"objectIds":[]})", {42}) == std::optional<bool>(false),
        "an empty valid ID query is evidence of expiration");
    for (const QByteArray& bad : {QByteArray("{}"), QByteArray("not json"),
        QByteArray(R"({"error":{},"objectIds":[]})"),
        QByteArray(R"({"exceededTransferLimit":true,"objectIds":[]})"),
        QByteArray(R"({"objectIds":["42"]})")}) {
        ok &= expect(!WeatherRadarSource::parseNoaaRasterAvailability(bad, {42}).has_value(),
            "untrustworthy availability responses must not expire cached weather");
    }
    const QUrlQuery availabilityQuery(WeatherRadarSource::noaaRasterAvailabilityUrl({7,42}));
    ok &= expect(availabilityQuery.queryItemValue("objectIds") == "7,42"
        && availabilityQuery.queryItemValue("returnIdsOnly") == "true"
        && !availabilityQuery.hasQueryItem("time"),
        "verify exactly the immutable raster IDs, not an estimated timestamp");
    ok &= expect(oneHourFrames.size() == 3,
                 "one-hour history should filter old observations and duplicates");
    ok &= expect(oneHourFrames.first().frameTime.toMSecsSinceEpoch()
                     == 1700001240000LL,
                 "timeline should preserve the exact oldest usable observation");
    ok &= expect(oneHourFrames.first().sampleTime.toMSecsSinceEpoch()
                     == 1700001540000LL,
                 "timeline should sample safely inside a finalized interval");
    ok &= expect(oneHourFrames.first().rasterIds
                     == QVector<qint64>({2, 10}),
                 "each frame must lock contemporaneous regional rasters");
    ok &= expect(oneHourFrames.last().frameTime.toMSecsSinceEpoch()
                     == 1700004840000LL,
                 "timeline must include the newest open-interval observation");
    ok &= expect(oneHourFrames.last().sampleTime == oneHourFrames.last().frameTime,
                 "an open interval locks its raster at its actual observation time");
    ok &= expect(oneHourFrames.last().rasterIds
                     == QVector<qint64>({5, 11}),
                 "regional raster selection must advance with the CONUS clock");
    ok &= expect(lockedQuery.queryItemValue(QStringLiteral("adjustAspectRatio"))
                     == QStringLiteral("false"),
                 "raw NOAA bytes must preserve the requested geographic bbox");
    const QByteArray refreshJson = R"({"features":[
        {"attributes":{"objectid":5,"idp_subset":"CONUS","idp_validtime":1700004840000,"idp_validendtime":1700005440000}},
        {"attributes":{"objectid":6,"idp_subset":"CONUS","idp_validtime":1700005440000,"idp_validendtime":1700005440000}},
        {"attributes":{"objectid":11,"idp_subset":"HAWAII","idp_validtime":1700004200000,"idp_validendtime":1700004200000}},
        {"attributes":{"objectid":12,"idp_subset":"HAWAII","idp_validtime":1700005400000,"idp_validendtime":1700005400000}},
        {"attributes":{"objectid":7,"idp_subset":"CONUS","idp_validtime":1700006040000,"idp_validendtime":1700005900000}}
    ]})";
    const QVector<WeatherRadarObservation> refreshedFrames =
        WeatherRadarSource::parseNoaaTimeline(refreshJson, 1);
    ok &= expect(refreshedFrames.size() == 2
                     && refreshedFrames.last().rasterIds == QVector<qint64>({6, 12})
                     && refreshedFrames.first().rasterIds == QVector<qint64>({5}),
                 "new catalogs advance the newest scan without carrying stale open regions or reversed intervals");

    const double world = kRadarWorldWidth;
    const QRectF us(-world * .32, -world * .2, world * .1, world * .1);
    for (const int copy : {-12, -2, -1, 0, 1, 3, 12}) {
        const QRectF view = us.translated(copy * world, 0);
        const QRectF canonical = weatherRadarCanonicalPlaybackBounds(view);
        ok &= expect(std::abs(canonical.left() - us.left()) < 1e-6
                         && std::abs(canonical.width() - us.width()) < 1e-6,
                     "a cropped storm export must be identical in every world copy");
        const QVector<double> offsets = weatherRadarVisibleWorldOffsets(canonical, view);
        ok &= expect(offsets.size() == 1 && offsets.first() == copy * world,
                     "cropped radar must follow its matching basemap world");
    }
    const QRectF wide(-world, -world / 2, 2 * world, world);
    const QRectF worldExport = weatherRadarCanonicalPlaybackBounds(wide);
    ok &= expect(worldExport == QRectF(-world / 2, -world / 2, world, world)
                     && weatherRadarVisibleWorldOffsets(worldExport, wide)
                         == QVector<double>({-world, 0, world}),
                 "a multi-world viewport must reuse one full-world export in every visible copy");
    const QRectF dateline(world * .45, -world * .2, world * .1, world * .1);
    const QRectF datelineExport = weatherRadarCanonicalPlaybackBounds(dateline);
    ok &= expect(datelineExport.width() == world
                     && weatherRadarVisibleWorldOffsets(datelineExport, dateline)
                         == QVector<double>({0, world}),
                 "both sides of the dateline must use their matching canonical radar pixels");
    ok &= expect(weatherRadarCanonicalPlaybackBounds(QRectF{}).isEmpty()
                     && weatherRadarVisibleWorldOffsets(QRectF{}, wide).isEmpty(),
                 "invalid geometry must not generate a world copy");
    ok &= expect(WeatherRadarSource::parseNoaaTimeline(
                     QByteArrayLiteral("{}"), 4).isEmpty(),
                 "malformed timeline payloads must not synthesize frames");

    const QVector<QDateTime> cadenceFrames{
        QDateTime::fromMSecsSinceEpoch(0, QTimeZone::UTC),
        QDateTime::fromMSecsSinceEpoch(6 * 60 * 1000, QTimeZone::UTC),
        QDateTime::fromMSecsSinceEpoch(14 * 60 * 1000, QTimeZone::UTC),
        QDateTime::fromMSecsSinceEpoch(20 * 60 * 1000, QTimeZone::UTC),
        QDateTime::fromMSecsSinceEpoch(28 * 60 * 1000, QTimeZone::UTC),
        // One and then two missing observations. Every segment retains the
        // same observed-time scale, including ordinary 6/8-minute intervals.
        QDateTime::fromMSecsSinceEpoch(42 * 60 * 1000, QTimeZone::UTC),
        QDateTime::fromMSecsSinceEpoch(63 * 60 * 1000, QTimeZone::UTC)
    };
    const QVector<int> segmentDurations =
        AetherSDR::weatherRadarPlaybackSegmentDurations(
            cadenceFrames, 10 * 60 * 1000, 450, 2400);
    ok &= expect(segmentDurations
                     == QVector<int>({600, 800, 600, 800, 1400, 2100}),
                 "every observation gap must retain the same time scale, including one/two missing frames");
    const QVector<QDateTime> shortCadenceFrames{
        QDateTime::fromMSecsSinceEpoch(0, QTimeZone::UTC),
        QDateTime::fromMSecsSinceEpoch(106000, QTimeZone::UTC),
        QDateTime::fromMSecsSinceEpoch(238000, QTimeZone::UTC),
        // Two missing two-minute observations: do not compress this longer
        // source interval into the same 450 ms as either ordinary interval.
        QDateTime::fromMSecsSinceEpoch(596000, QTimeZone::UTC)
    };
    const QVector<int> shortDurations = AetherSDR::weatherRadarPlaybackSegmentDurations(
        shortCadenceFrames, 10 * 60 * 1000, 450, 2400);
    ok &= expect(shortDurations == QVector<int>({450, 560, 1520}),
                 "duration bounds must select one history-wide time scale, not independently clamp unequal short gaps");
    const QVector<QDateTime> extremeCadenceFrames{
        QDateTime::fromMSecsSinceEpoch(0, QTimeZone::UTC),
        QDateTime::fromMSecsSinceEpoch(1000, QTimeZone::UTC),
        QDateTime::fromMSecsSinceEpoch(2400000, QTimeZone::UTC)
    };
    ok &= expect(AetherSDR::weatherRadarPlaybackSegmentDurations(
                     extremeCadenceFrames, 10 * 60 * 1000, 450, 2400)
                     == QVector<int>({1, 2400}),
                 "incompatible min/max bounds must not magnify a tiny source gap into a very long loop");
    const AetherSDR::WeatherRadarPlaybackPosition beforeBoundary =
        AetherSDR::weatherRadarPlaybackPosition(
            599, 7, segmentDurations, 1000);
    const AetherSDR::WeatherRadarPlaybackPosition atBoundary =
        AetherSDR::weatherRadarPlaybackPosition(
            600, 7, segmentDurations, 1000);
    ok &= expect(beforeBoundary.fromIndex == 0
                     && beforeBoundary.toIndex == 1
                     && beforeBoundary.progress > 0.99,
                 "playback must approach the next frame continuously");
    ok &= expect(atBoundary.fromIndex == 1
                     && atBoundary.toIndex == 2
                     && atBoundary.progress == 0.0,
                 "playback must cross a frame boundary without a dwell");
    const AetherSDR::WeatherRadarPlaybackPosition loopHold =
        AetherSDR::weatherRadarPlaybackPosition(
            6300, 7, segmentDurations, 1000);
    const AetherSDR::WeatherRadarPlaybackPosition loopRestart =
        AetherSDR::weatherRadarPlaybackPosition(
            7300, 7, segmentDurations, 1000);
    ok &= expect(loopHold.fromIndex == 6 && loopHold.toIndex == 6
                     && loopHold.progress == 1.0,
                 "the one-second pause must retain the last frame");
    ok &= expect(loopRestart.fromIndex == 0 && loopRestart.toIndex == 1
                     && loopRestart.progress == 0.0,
                 "the next loop must restart at the oldest observation");

    constexpr qint64 kPlaybackPresentationTimeoutMs = 250;
    // Production playback now displays original observations only. Sample
    // EVERY millisecond, not just the knots: no blend or second observation
    // may sneak into a dwell, the final hold or the next loop.
    const QVector<int> originalDurations{450, 560, 1520};
    bool originalsOnly = true;
    for (qint64 elapsed = 0; elapsed < 2 * 3530; ++elapsed) {
        const qint64 phase = elapsed % 3530;
        const int expected = phase < 450 ? 0
            : phase < 1010 ? 1 : phase < 2530 ? 2 : 3;
        const auto original = AetherSDR::weatherRadarObservationPlaybackPosition(
            elapsed, 4, originalDurations, 1000);
        originalsOnly &= original.valid && original.fromIndex == expected
            && original.toIndex == expected && original.progress == 0.0;
    }
    ok &= expect(originalsOnly,
                 "every playback instant must select exactly one original NOAA image, with a visible final hold");
    ok &= expect(AetherSDR::weatherRadarPlaybackScaledDurations(
                     {600, 800, 1400}, 50) == QVector<int>({1200, 1600, 2800})
                 && AetherSDR::weatherRadarPlaybackScaledDurations(
                     {600, 800, 1400}, 200) == QVector<int>({300, 400, 700})
                 && AetherSDR::weatherRadarPlaybackScaledDurations(
                     {600, 800, 1400}, 400) == QVector<int>({150, 200, 350})
                 && AetherSDR::weatherRadarPlaybackScaledDurations(
                     {600, 800, 1400}, 500) == QVector<int>({120, 160, 280})
                 && AetherSDR::weatherRadarPlaybackSpeedPercent(501) == 100
                 && AetherSDR::weatherRadarPlaybackSpeedPercent(0) == 100
                 && AetherSDR::weatherRadarPlaybackSpeedPercent(999) == 100
                 && AetherSDR::weatherRadarPlaybackSpeedPercent(137) == 137
                 && AetherSDR::weatherRadarPlaybackScaledDurations(
                     {1370}, 137) == QVector<int>({1000}),
                 "speeds must scale all original dwells and reject unsupported saved values");
    bool speedPreservesObservation = true;
    for (const int speed : {25, 50, 73, 100, 137, 200, 333, 400, 500}) {
        const QVector<int> scaled = AetherSDR::weatherRadarPlaybackScaledDurations(
            originalDurations, speed);
        const qint64 scaledTotal = scaled.at(0) + scaled.at(1) + scaled.at(2);
        for (qint64 elapsed = 0; elapsed < 2 * 3530; ++elapsed) {
            const qint64 rebased = AetherSDR::weatherRadarPlaybackElapsedAtSpeed(
                elapsed, originalDurations, scaled, 1000);
            const auto before = AetherSDR::weatherRadarObservationPlaybackPosition(
                elapsed, 4, originalDurations, 1000);
            const auto after = AetherSDR::weatherRadarObservationPlaybackPosition(
                rebased, 4, scaled, 1000);
            speedPreservesObservation &= before.fromIndex == after.fromIndex
                && after.fromIndex == after.toIndex && after.progress == 0.0;
            if (elapsed % 3530 >= 2530) {
                speedPreservesObservation &= rebased - scaledTotal
                    == elapsed % 3530 - 2530;
            }
        }
    }
    ok &= expect(speedPreservesObservation,
                 "changing speed must preserve the original on screen and elapsed final-hold time in every loop");
    ok &= expect(AetherSDR::weatherRadarPlaybackElapsedAtSpeed(
                     1000, {600, 800, 1400}, {300, 400, 700}, 1000) == 500,
                 "speed change halfway through a dwell must retain its halfway position");
    ok &= expect(AetherSDR::weatherRadarObservationClampToBoundary(
                     400, 1700, originalDurations, 1000) == 450
                 && AetherSDR::weatherRadarObservationClampToBoundary(
                     450, 1700, originalDurations, 1000) == 1010
                 && AetherSDR::weatherRadarObservationClampToBoundary(
                     2530, 4000, originalDurations, 1000) == 3530,
                 "a slow paint must not skip an original observation or the final hold");
    ok &= expect(!AetherSDR::weatherRadarObservationPlaybackPosition(
                     0, 1, {}, 1000).valid
                 && !AetherSDR::weatherRadarObservationPlaybackPosition(
                     -1, 4, originalDurations, 1000).valid
                 && !AetherSDR::weatherRadarObservationPlaybackPosition(
                     0, 4, {450, 0, 1520}, 1000).valid,
                 "invalid original-image timelines must not fabricate observations");
    // Presentation acknowledgement order must not change the observed-time
    // speed. The previous cadence threw away all but one queued 16 ms tick,
    // so the same movie slowed whenever a renderer needed two/three ticks.
    AetherSDR::WeatherRadarPlaybackCadence delayedClock;
    delayedClock.reset();
    const std::optional<qint64> delayedStart = delayedClock.timerTick(16, 250, 16);
    const quint64 delayedToken = delayedClock.beginPresentation(
        delayedStart.value_or(-1));
    delayedClock.timerTick(32, 250, 16);
    delayedClock.timerTick(48, 250, 16);
    const std::optional<qint64> delayedNext = delayedClock.completePresentation(delayedToken);
    ok &= expect(delayedNext == 48,
                 "ordinary delayed presentation must retain all elapsed time, not just one queued tick");
    const quint64 delayedNextToken = delayedClock.beginPresentation(
        delayedNext.value_or(-1));
    delayedClock.completePresentation(delayedNextToken);
    ok &= expect(delayedClock.timerTick(64, 250, 16) == 64,
                 "presentation latency must not permanently slow the playback clock");

    AetherSDR::WeatherRadarPlaybackCadence timerBeforeAck;
    timerBeforeAck.reset();
    const std::optional<qint64> firstTimerPresentation =
        timerBeforeAck.timerTick(16, 250, 16);
    ok &= expect(firstTimerPresentation == 16,
                 "the first cadence tick must advance one timer interval");
    const quint64 firstPresentationSequence =
        timerBeforeAck.beginPresentation(
            firstTimerPresentation.value_or(-1));
    ok &= expect(firstPresentationSequence != 0
                     && timerBeforeAck.presentationPending(),
                 "an accepted presentation must remain pending until paint");
    ok &= expect(!timerBeforeAck.timerTick(32, 250, 16).has_value(),
                 "a timer queued before paint acknowledgement must not overwrite the in-flight frame");
    const std::optional<qint64> presentationAfterQueuedTick =
        timerBeforeAck.completePresentation(firstPresentationSequence);
    ok &= expect(presentationAfterQueuedTick == 32,
                 "paint acknowledgement must immediately drain the queued timer interval");
    const quint64 secondPresentationSequence =
        timerBeforeAck.beginPresentation(
            presentationAfterQueuedTick.value_or(-1));
    ok &= expect(!timerBeforeAck.completePresentation(
                      secondPresentationSequence).has_value()
                     && timerBeforeAck.presentedElapsedMs() == 32,
                 "drained cadence must commit without inventing another step");

    AetherSDR::WeatherRadarPlaybackCadence ackBeforeTimer;
    ackBeforeTimer.reset();
    const std::optional<qint64> firstAckFirstPresentation =
        ackBeforeTimer.timerTick(16, 250, 16);
    const quint64 ackFirstSequence =
        ackBeforeTimer.beginPresentation(
            firstAckFirstPresentation.value_or(-1));
    ok &= expect(!ackBeforeTimer.completePresentation(
                      ackFirstSequence).has_value(),
                 "an acknowledgement with no queued tick must not schedule a duplicate frame");
    ok &= expect(ackBeforeTimer.timerTick(32, 250, 16)
                     == presentationAfterQueuedTick,
                 "timer-before-ack and ack-before-timer order must produce the same next elapsed time");

    AetherSDR::WeatherRadarPlaybackCadence boundedBacklog;
    boundedBacklog.reset();
    const std::optional<qint64> backlogFirst =
        boundedBacklog.timerTick(16, 250, 16);
    const quint64 backlogSequence =
        boundedBacklog.beginPresentation(backlogFirst.value_or(-1));
    boundedBacklog.timerTick(32, 250, 16);
    boundedBacklog.timerTick(64, 250, 16);
    boundedBacklog.timerTick(264, 250, 16);
    ok &= expect(boundedBacklog.completePresentation(backlogSequence) == 264,
                 "multiple delayed paints must coalesce to the latest movie time without changing speed");

    AetherSDR::WeatherRadarPlaybackCadence eventLoopStall;
    eventLoopStall.reset();
    ok &= expect(eventLoopStall.timerTick(200, 250, 16) == 200,
                 "ordinary timer lateness must not silently slow the movie clock");

    AetherSDR::WeatherRadarPlaybackCadence suspendedClock;
    suspendedClock.reset(100, 0);
    ok &= expect(suspendedClock.timerTick(10000, 250, 16) == 116,
                 "resuming a suspended UI must not replay ten seconds of unseen animation at once");
    const quint64 resumedToken = suspendedClock.beginPresentation(116);
    suspendedClock.completePresentation(resumedToken);
    ok &= expect(suspendedClock.timerTick(10016, 250, 16) == 132,
                 "resuming must permanently rebase the clock, without a deferred catch-up burst");

    AetherSDR::WeatherRadarPlaybackCadence overflowClock;
    overflowClock.reset(std::numeric_limits<qint64>::max() - 5, 0);
    ok &= expect(overflowClock.timerTick(16, 250, 16)
                     == std::numeric_limits<qint64>::max(),
                 "the absolute movie clock must saturate rather than overflow");

    AetherSDR::WeatherRadarPlaybackCadence staleAcknowledgement;
    staleAcknowledgement.reset();
    const std::optional<qint64> staleFirst =
        staleAcknowledgement.timerTick(16, 250, 16);
    const quint64 currentSequence =
        staleAcknowledgement.beginPresentation(staleFirst.value_or(-1));
    staleAcknowledgement.timerTick(32, 250, 16);
    ok &= expect(!staleAcknowledgement.completePresentation(
                      currentSequence + 1).has_value()
                     && staleAcknowledgement.presentationPending()
                     && staleAcknowledgement.presentedElapsedMs() == 0,
                 "a stale renderer acknowledgement must not commit or drain playback");
    ok &= expect(staleAcknowledgement.completePresentation(
                      currentSequence) == 32,
                 "the matching acknowledgement must retain the queued advance");

    AetherSDR::WeatherRadarPlaybackCadence lostAcknowledgement;
    lostAcknowledgement.reset();
    const std::optional<qint64> lostFirst =
        lostAcknowledgement.timerTick(16, 250, 16,
            kPlaybackPresentationTimeoutMs);
    const quint64 lostSequence =
        lostAcknowledgement.beginPresentation(lostFirst.value_or(-1));
    ok &= expect(!lostAcknowledgement.timerTick(265, 250, 16,
                      kPlaybackPresentationTimeoutMs).has_value(),
                 "a pending presentation must remain owned until its timeout");
    const std::optional<qint64> retriedElapsed =
        lostAcknowledgement.timerTick(266, 250, 16,
            kPlaybackPresentationTimeoutMs);
    const quint64 retrySequence =
        lostAcknowledgement.beginPresentation(
            retriedElapsed.value_or(-1));
    ok &= expect(retriedElapsed == lostFirst
                     && retrySequence != 0
                     && retrySequence != lostSequence
                     && lostAcknowledgement.presentationPending(),
                 "a lost acknowledgement must retry the exact unseen position with a new token");
    ok &= expect(!lostAcknowledgement.completePresentation(
                      lostSequence).has_value()
                     && lostAcknowledgement.presentationPending()
                     && lostAcknowledgement.presentedElapsedMs() == 0,
                 "a late acknowledgement must not complete the replacement presentation");
    lostAcknowledgement.completePresentation(retrySequence);
    ok &= expect(!lostAcknowledgement.presentationPending()
                     && lostAcknowledgement.presentedElapsedMs()
                            == lostFirst.value_or(-1),
                 "only the replacement acknowledgement may commit the retried position");

    AetherSDR::WeatherRadarPlaybackCadence rejectedPresentation;
    rejectedPresentation.reset();
    const std::optional<qint64> rejectedElapsed =
        rejectedPresentation.timerTick(16, 250, 16);
    const quint64 rejectedSequence =
        rejectedPresentation.beginPresentation(rejectedElapsed.value_or(-1));
    rejectedPresentation.timerTick(32, 250, 16);
    rejectedPresentation.rejectPresentation(rejectedSequence);
    ok &= expect(!rejectedPresentation.presentationPending()
                     && rejectedPresentation.presentedElapsedMs() == 0
                     && rejectedPresentation.timerTick(48, 250, 16) == 16,
                 "a renderer rejection must not commit or later skip the unseen frame");

    AetherSDR::WeatherRadarPlaybackCadence resetCadence;
    resetCadence.reset();
    const std::optional<qint64> resetFirst =
        resetCadence.timerTick(16, 250, 16);
    const quint64 resetSequence =
        resetCadence.beginPresentation(resetFirst.value_or(-1));
    resetCadence.timerTick(32, 250, 16);
    resetCadence.reset(100, 50);
    const std::optional<qint64> afterResetElapsed =
        resetCadence.timerTick(66, 250, 16);
    const quint64 afterResetSequence =
        resetCadence.beginPresentation(afterResetElapsed.value_or(-1));
    ok &= expect(!resetCadence.completePresentation(
                      resetSequence).has_value()
                     && afterResetSequence != resetSequence
                     && resetCadence.presentationPending()
                     && afterResetElapsed == 116,
                 "reset must discard stale in-flight and deferred cadence state");
    resetCadence.completePresentation(afterResetSequence);

    AetherSDR::WeatherRadarPlaybackCadence rebasedCadence;
    rebasedCadence.reset(8200, 0);
    ok &= expect(rebasedCadence.timerTick(0, 250, 16) == 8200,
                 "the cadence must initially retain the current loop elapsed time");
    rebasedCadence.rebaseElapsed(0);
    const quint64 rebasedSequence = rebasedCadence.beginPresentation(0);
    rebasedCadence.completePresentation(rebasedSequence);
    ok &= expect(rebasedCadence.presentedElapsedMs() == 0,
                 "adopting a replacement trajectory must rebase the loop without stale backlog");

    AetherSDR::WeatherRadarPlaybackCadence boundaryCadence;
    boundaryCadence.reset(584, 0);
    const std::optional<qint64> boundaryElapsed =
        boundaryCadence.timerTick(16, 250, 16);
    const AetherSDR::WeatherRadarPlaybackPosition cadenceBoundary =
        AetherSDR::weatherRadarPlaybackPosition(
            boundaryElapsed.value_or(-1), 7, segmentDurations, 1000);
    const quint64 boundarySequence =
        boundaryCadence.beginPresentation(boundaryElapsed.value_or(-1));
    boundaryCadence.timerTick(32, 250, 16);
    const std::optional<qint64> afterBoundaryElapsed =
        boundaryCadence.completePresentation(boundarySequence);
    const AetherSDR::WeatherRadarPlaybackPosition cadenceAfterBoundary =
        AetherSDR::weatherRadarPlaybackPosition(
            afterBoundaryElapsed.value_or(-1), 7, segmentDurations, 1000);
    ok &= expect(cadenceBoundary.fromIndex == 1
                     && cadenceBoundary.toIndex == 2
                     && cadenceBoundary.progress == 0.0
                     && afterBoundaryElapsed.value_or(-1)
                            - boundaryElapsed.value_or(-1) == 16
                     && cadenceAfterBoundary.fromIndex == 1
                     && cadenceAfterBoundary.toIndex == 2
                     && cadenceAfterBoundary.progress > 0.0,
                 "a queued acknowledgement at a NOAA knot must advance one frame without a dwell or double step");

    AetherSDR::WeatherRadarPlaybackCadence missingFrameCadence;
    missingFrameCadence.reset(2800, 0);
    const std::optional<qint64> missingFrameElapsed =
        missingFrameCadence.timerTick(16, 250, 16);
    const AetherSDR::WeatherRadarPlaybackPosition missingFramePosition =
        AetherSDR::weatherRadarPlaybackPosition(
            missingFrameElapsed.value_or(-1), 7, segmentDurations, 1000);
    const quint64 missingFrameSequence =
        missingFrameCadence.beginPresentation(
            missingFrameElapsed.value_or(-1));
    missingFrameCadence.timerTick(32, 250, 16);
    const std::optional<qint64> missingFrameNextElapsed =
        missingFrameCadence.completePresentation(missingFrameSequence);
    ok &= expect(missingFramePosition.fromIndex == 4
                     && missingFramePosition.toIndex == 5
                     && qFuzzyCompare(missingFramePosition.progress, 16.0 / 1400.0)
                     && missingFrameNextElapsed.value_or(-1)
                            - missingFrameElapsed.value_or(-1) == 16,
                 "a missing NOAA frame must retain its longer segment while presentation cadence stays uniform");

    const QSet<int> activeDecodeWindow =
        AetherSDR::weatherRadarPlaybackDecodedWindow(3, 12, 5);
    ok &= expect(activeDecodeWindow.contains(3)
                     && activeDecodeWindow.contains(4),
                 "decode look-ahead must retain the complete active pair");
    ok &= expect(activeDecodeWindow
                     == QSet<int>({2, 3, 4, 5, 6, 7}),
                 "decode look-ahead must remain bounded around its source");
    const QSet<int> wrappedDecodeWindow =
        AetherSDR::weatherRadarPlaybackDecodedWindow(0, 12, 5);
    ok &= expect(wrappedDecodeWindow.contains(11)
                     && wrappedDecodeWindow.contains(0)
                     && wrappedDecodeWindow.contains(1),
                 "decode look-ahead must retain both sides of loop restart");

    for (qint64 elapsed = 0; elapsed < 20000; elapsed += 137) {
        const AetherSDR::WeatherRadarPlaybackPosition prefixPosition =
            AetherSDR::weatherRadarPlaybackPosition(
                elapsed, 5, segmentDurations, 1000);
        ok &= expect(prefixPosition.valid
                         && prefixPosition.fromIndex < 5
                         && prefixPosition.toIndex < 5,
                     "a ready prefix must never expose a pending frame");
    }

    return ok ? 0 : 1;
}
