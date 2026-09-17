#include "gui/map/MapDisplayWidget.h"
#include "gui/map/WeatherRadarController.h"
#include "gui/map/WeatherRadarProvenance.h"
#include "gui/map/WeatherRadarLegend.h"
#include "gui/map/MapProviderNetworkAccessManager.h"
#include "gui/map/CityLightsItem.h"
#include "gui/map/GlobeMapView.h"
#include "gui/map/WeatherRadarTexture.h"
#include "gui/map/WeatherRadarTileLayer.h"
#include "gui/map/RegionalRadarComposite.h"
#include "gui/map/OperaRadarNetwork.h"

#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVMapQGView.h>
#include <QGeoView/QGVProjection.h>
#include <QGeoView/QGVLayerTiles.h>
#include <QGeoView/Raster/QGVImage.h>
#include <QApplication>
#include <QBuffer>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <QUrlQuery>

#include <cstring>

namespace AetherSDR {

class InjectedTileLayer final : public QGVLayerTiles {
public:
    int zoom{2};
    using QGVLayerTiles::tileUncoveredPath;
    void process() { onUpdate(); }
    void deliver(const QGV::GeoTilePos& position)
    {
        auto* image = new QGVImage();
        image->setGeometry(position.toGeoRect());
        QImage pixels(16, 16, QImage::Format_ARGB32_Premultiplied);
        pixels.fill(Qt::transparent); // Clear pixels ALSO replace stale rain.
        image->loadImage(pixels);
        onTile(position, image);
    }
protected:
    int minZoomlevel() const override { return 0; }
    int maxZoomlevel() const override { return 10; }
    int scaleToZoom(double) const override { return zoom; }
    void request(const QGV::GeoTilePos&) override {}
    void cancel(const QGV::GeoTilePos&) override {}
};

class ControlledRadarReply final : public QNetworkReply {
public:
    ControlledRadarReply(const QNetworkRequest& request, QObject* parent)
        : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    void abort() override
    {
        if (isFinished()) {
            return; // Match Qt: retiring a completed reply is not a new failure.
        }
        setError(OperationCanceledError, QStringLiteral("Canceled"));
        setFinished(true);
        emit finished();
    }
    void complete(bool fail = false, const QColor& fill = {})
    {
        if (isFinished()) {
            return;
        }
        if (fail) {
            setError(TimeoutError, QStringLiteral("Injected slow NOAA response"));
        } else {
            const QStringList size = QUrlQuery(url()).queryItemValue("size").split(',');
            const QSize pixels(size.value(0).toInt(), size.value(1).toInt());
            QImage image(pixels, WeatherRadarTexture::kImageFormat);
            image.fill(Qt::transparent);
            // Nonempty original data; distinct QImage identities detect upgrade.
            image.setPixelColor(pixels.width() / 2, pixels.height() / 2, Qt::red);
            if (fill.isValid()) {
                image.fill(fill);
            }
            QBuffer buffer(&m_bytes);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
        }
        setFinished(true);
        emit readyRead();
        emit finished();
    }
    qint64 bytesAvailable() const override { return m_bytes.size() + QNetworkReply::bytesAvailable(); }
    void completeJson(const QByteArray& bytes)
    {
        if (isFinished()) {
            return;
        }
        m_bytes = bytes;
        setFinished(true);
        emit readyRead();
        emit finished();
    }
protected:
    qint64 readData(char* data, qint64 maximum) override
    {
        const qint64 count = std::min(maximum, qint64(m_bytes.size()));
        if (count == 0) {
            return -1;
        }
        std::memcpy(data, m_bytes.constData(), size_t(count));
        m_bytes.remove(0, count);
        return count;
    }
private:
    QByteArray m_bytes;
};

class ControlledRadarNetwork final : public QNetworkAccessManager {
public:
    QList<QPointer<ControlledRadarReply>> exports;
    QList<QPointer<ControlledRadarReply>> allReplies;
    QList<QUrl> requests;
    ControlledRadarReply* pending(const QUrl& url = {}) const
    {
        for (const auto& reply : exports) {
            if (reply && !reply->isFinished() && (url.isEmpty() || reply->url() == url)) {
                return reply;
            }
        }
        return nullptr;
    }
    ControlledRadarReply* pendingValidation() const
    {
        for (const auto& reply : allReplies) {
            if (reply && !reply->isFinished()
                && QUrlQuery(reply->url()).queryItemValue("returnIdsOnly") == "true") {
                return reply;
            }
        }
        return nullptr;
    }
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        // All networking (including the map's basemap) is intercepted here.
        // No socket, TLS server, live weather, or third-party radio peer.
        auto* reply = new ControlledRadarReply(request, this);
        allReplies.append(reply);
        if (request.url().path().endsWith("exportImage")) {
            exports.append(reply);
            requests.append(request.url());
        }
        return reply;
    }
};

class WeatherRadarLoadingTest final : public QObject {
    Q_OBJECT
private:
    static void prepare(MapDisplayWidget& map, ControlledRadarNetwork& network,
                        int count = 6, bool nativeFlat = false)
    {
        map.m_weatherRadar->m_weatherRadarNetwork = &network;
        QGVMap* flat = map.m_flatView->findChild<QGVMap*>();
        if (!nativeFlat) {
            flat->geoView()->setViewport(new QWidget()); // Production raster fallback.
        }
        map.resize(600, 400);
        map.show();
        QCoreApplication::processEvents();
        flat->cameraTo(QGVCameraActions(flat).scaleTo(.0001)
            .moveTo(QPointF(-1.05e7, -4.0e6)), false);
        QCoreApplication::processEvents();
        map.m_weatherRadar->m_weatherRadarVisible = true;
        map.m_weatherRadar->m_weatherRadarPlaybackRequested = true;
        const QDateTime start = QDateTime::currentDateTimeUtc().addSecs(-count * 300);
        for (int i = 0; i < count; ++i) {
            map.m_weatherRadar->m_frames.append(WeatherRadarFrame{start.addSecs(i * 300)});
            map.m_weatherRadar->m_frames.last().sampleTime = start.addSecs(i * 300 + 150);
            map.m_weatherRadar->m_frames.last().rasterIds = QVector<qint64>{100 + i};
        }
        map.m_weatherRadar->bufferWeatherRadarFrames();
    }
    static void finishAll(MapDisplayWidget& map, ControlledRadarNetwork& network)
    {
        QElapsedTimer deadline;
        deadline.start();
        while (!map.m_weatherRadar->m_weatherRadarNetworkRequestsComplete && deadline.elapsed() < 5000) {
            if (auto* reply = network.pending()) {
                reply->complete();
            }
            QTest::qWait(10);
        }
    }

private slots:
    void coverageTooltipClearsWhenLeavingOrDisabling()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        GlobeMapView globe;
        globe.resize(600, 400);
        RadarSite site;
        site.id = QStringLiteral("TEST");
        globe.setRadarSites({site}, true);
        globe.m_overlayMatricesValid = true;
        globe.m_overlayModel.setToIdentity();
        globe.m_overlayViewProjection.setToIdentity();
        QPointF point;
        QVERIFY(globe.projectPoint(globe.geoPoint(0, 0),
            globe.m_overlayModel, globe.m_overlayViewProjection, &point));
        globe.updateHover(point);
        QVERIFY(!globe.m_hoverCard->isHidden());
        globe.updateHover(QPointF(0, 0));
        QVERIFY(globe.m_hoverCard->isHidden());
        globe.updateHover(point);
        QVERIFY(!globe.m_hoverCard->isHidden());
        globe.setRadarSites({site}, false);
        QVERIFY(globe.m_hoverCard->isHidden());
        QGV::setNetworkManager(nullptr);
    }

    void playbackCoverageRemainsPinnedDuringRebuffer()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        MapDisplayWidget map;
        map.m_weatherRadar->m_weatherRadarNetwork = &network;
        map.m_weatherRadar->m_weatherRadarSource = WeatherRadarSource::composite(15);
        QGVMap* flat = map.m_flatView->findChild<QGVMap*>();
        flat->geoView()->setViewport(new QWidget());
        map.resize(600, 400);
        map.show();
        QCoreApplication::processEvents();
        map.m_weatherRadar->m_weatherRadarVisible = true;
        map.m_weatherRadar->m_weatherRadarPlaybackRequested = true;
        const qint64 stamp = QDateTime::currentSecsSinceEpoch() / 600 * 600 - 600;
        for (int providers : {8, 7}) {
            map.m_weatherRadar->cancelWeatherRadarFrameRequests();
            const QByteArray catalog = QJsonDocument(QJsonObject{
                {QStringLiteral("times"), QJsonArray{stamp - 600, stamp}},
                {QStringLiteral("providers"), providers}}).toJson();
            QVERIFY(map.m_weatherRadar->useWeatherRadarTimeline(catalog, 2));
            const auto checkRequests = [&] {
                int pending = 0;
                for (const auto& reply : network.allReplies) {
                    if (!reply || reply->isFinished() || reply->url().scheme() != "radar-composite") {
                        continue;
                    }
                    QCOMPARE(QUrlQuery(reply->url()).queryItemValue("providers").toInt(), providers);
                    ++pending;
                }
                QVERIFY(pending > 0);
            };
            checkRequests();
            for (const WeatherRadarFrame& frame : map.m_weatherRadar->m_frames) {
                QCOMPARE(frame.providers, providers);
            }
            map.m_weatherRadar->cancelWeatherRadarFrameRequests();
            map.m_weatherRadar->bufferWeatherRadarFrames(); // Same path used by a zoom refresh.
            checkRequests();
            QCOMPARE(map.m_weatherRadar->m_weatherRadarSource.enabledProviders(), 15); // Preserve user settings.
        }
        const QByteArray primaryCatalog = QJsonDocument(QJsonObject{
            {QStringLiteral("times"), QJsonArray{stamp - 600, stamp}},
            {QStringLiteral("providers"), 8}}).toJson();
        map.m_weatherRadar->cancelWeatherRadarFrameRequests();
        QVERIFY(map.m_weatherRadar->useWeatherRadarTimeline(primaryCatalog, 2));
        const auto failedReplies = network.allReplies;
        for (const auto& reply : failedReplies) {
            if (reply && !reply->isFinished() && reply->url().scheme() == "radar-composite") {
                reply->complete(true);
            }
        }
        QCOMPARE(map.m_weatherRadar->m_weatherRadarPlaybackProviders, 7);
        QVERIFY(map.m_weatherRadar->m_weatherRadarTimelineReply != nullptr);
        QCOMPARE(QUrlQuery(map.m_weatherRadar->m_weatherRadarTimelineReply->url())
            .queryItemValue("providers").toInt(), 7); // Entire fallback movie, not mixed frames.
        GlobeMapView globe;
        globe.resize(2000, 2000);
        const QSize size = globe.weatherRadarPlaybackSize(QRectF(0, 0, 3000000, 3000000));
        QVERIFY(qint64(size.width()) * size.height() <= kMaximumWeatherRadarPixels);
        map.stopWeatherRadarAnimation();
        QGV::setNetworkManager(nullptr);
    }

    void globeCoverageMovesWithRenderedSurface()
    {
        if (!qEnvironmentVariableIsSet("AETHERSDR_TEST_RADAR_GL")) {
            QSKIP("Opt in with AETHERSDR_TEST_RADAR_GL=1 and a native GUI platform");
        }
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        GlobeMapView globe;
        globe.resize(600, 400);
        globe.show();
        QTRY_VERIFY(globe.isValid());
        globe.cancelTileRequests();
        globe.m_detailSelectionDirty = false;
        globe.m_atlas.fill(Qt::black);
        globe.m_atlasDirty = true;
        globe.m_terminatorVisible = false;
        globe.m_basemapDarkEnabled = false;
        RadarSite site;
        site.rangeKm = 460;
        site.ring = radarRangeRing(0, 0, site.rangeKm);
        globe.setRadarSites({site}, true);
        globe.m_navigation.reset(0, 0);
        const auto shade = [](const QColor& pixel) {
            return std::max({pixel.red(), pixel.green(), pixel.blue()});
        };
        // grabFramebuffer reads the globe pass alone, without processing the
        // separate vector QWidget's queued paint. Coverage must already be in
        // this frame at its new geographic location after every rotation.
        QImage frame = globe.grabFramebuffer();
        const QPoint center(frame.width() / 2, frame.height() / 2);
        const int single = shade(frame.pixelColor(center));
        QVERIFY(single > 2 && single <= 12);
        globe.setRadarSites({site, site}, true);
        frame = globe.grabFramebuffer();
        QCOMPARE(shade(frame.pixelColor(center)), single); // No darker overlaps.
        const GLuint resident = globe.m_radarCoverageBuffer.bufferId();
        for (int longitude = 4; longitude <= 60; longitude += 4) {
            globe.m_navigation.reset(0, longitude);
            frame = globe.grabFramebuffer();
            QPointF point;
            QVERIFY(globe.projectPoint(globe.geoPoint(0, 0),
                globe.m_overlayModel, globe.m_overlayViewProjection, &point));
            const QPoint pixel = (point * globe.devicePixelRatioF()).toPoint();
            QVERIFY(frame.rect().contains(pixel));
            QVERIFY(shade(frame.pixelColor(pixel)) > 2);
            if (longitude > 8) {
                QCOMPARE(shade(frame.pixelColor(center)), 0); // No trailing footprint.
            }
            QCOMPARE(globe.m_radarCoverageBuffer.bufferId(), resident);
            QVERIFY(!globe.m_radarCoverageDirty);
        }
        globe.m_navigation.reset(0, 180);
        frame = globe.grabFramebuffer();
        QCOMPARE(shade(frame.pixelColor(center)), 0); // No far-side coverage.
        globe.m_navigation.reset(0, 0);
        globe.setRadarSites({site}, false);
        frame = globe.grabFramebuffer();
        QCOMPARE(shade(frame.pixelColor(center)), 0);
        QGV::setNetworkManager(nullptr);
    }

    void globeAdmitsNativeRegionalLiveTiles_data()
    {
        QTest::addColumn<int>("providers");
        QTest::newRow("regional-512") << 7;
        QTest::newRow("global-512") << 15;
    }
    void globeAdmitsNativeRegionalLiveTiles()
    {
        QFETCH(int,providers);
        const int tileSize=512;
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        GlobeMapView globe;
        globe.setWeatherRadarSource(WeatherRadarSource::composite(providers));
        globe.setWeatherRadarVisible(true);
        bool sawNativeTile = false;
        QImage pixels(tileSize,tileSize,QImage::Format_ARGB32_Premultiplied);
        pixels.fill(Qt::green);
        QByteArray png;
        QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); pixels.save(&buffer,"PNG");
        for (int pass = 0; pass < 100; ++pass) {
            const auto replies = network.allReplies;
            bool completed = false;
            for (const auto& reply : replies) {
                if (reply && !reply->isFinished()) {
                    sawNativeTile |= reply->url().scheme() == QStringLiteral("radar-composite");
                    reply->completeJson(png);
                    completed = true;
                }
            }
            if (!completed) { break; }
        }
        QGV::setNetworkManager(nullptr);
        QVERIFY2(sawNativeTile,"The globe must dispatch its in-process radar tile requests");
        QCOMPARE(globe.m_weatherRadarAtlas.size(),QSize(tileSize*4,tileSize*4));
        QCOMPARE(globe.m_weatherRadarAtlas.pixelColor(128,128),QColor(Qt::green));
        QCOMPARE(globe.m_weatherRadarAtlas.pixelColor(tileSize*4-128,tileSize*4-128),QColor(Qt::green));
    }

    void globePlaybackOverviewKeepsFullAtlasResolution()
    {
        GlobeMapView globe;
        const QRectF world(-kRadarMercatorExtent,-kRadarMercatorExtent,kRadarWorldWidth,kRadarWorldWidth);
        globe.resize(640,480);
        QCOMPARE(globe.weatherRadarPlaybackSize(world),QSize(2048,2048));
        globe.resize(3840,2160);
        QCOMPARE(globe.weatherRadarPlaybackSize(world),QSize(2048,2048));
    }

    void regionalCompositeKeepsIndependentCoverage()
    {
        QVector<QImage> images(3);
        for (QImage& image : images) { image = QImage(3,1,QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent); }
        images[0].setPixelColor(0,0,Qt::red);
        images[1].setPixelColor(0,0,Qt::green);
        images[1].setPixelColor(1,0,Qt::green);
        images[2].setPixelColor(2,0,Qt::blue);
        const QImage all = composeRegionalRadar(images,QSize(3,1));
        QCOMPARE(all.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(all.pixelColor(1,0),QColor(Qt::green));
        QCOMPARE(all.pixelColor(2,0),QColor(Qt::blue));
        images[0] = {};
        const QImage partial = composeRegionalRadar(images,QSize(3,1));
        QCOMPARE(partial.pixelColor(0,0),QColor(Qt::green));
        QCOMPARE(partial.pixelColor(2,0),QColor(Qt::blue));
    }

    void providerIdentitySurvivesPngCacheAndTexturePreparation()
    {
        QVector<QImage> images(4);
        images[0] = QImage(2, 2, QImage::Format_ARGB32);
        images[0].fill(Qt::transparent);
        images[2] = images[0];
        const WeatherRadarSource source = WeatherRadarSource::composite(15);
        QImage result = composeRegionalRadar(images, QSize(2, 2));
        QCOMPARE(radarImageProviders(result, source), 5);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(result.save(&buffer, "PNG"));
        QImage decoded;
        QVERIFY(decoded.loadFromData(bytes, "PNG"));
        QCOMPARE(radarImageProviders(WeatherRadarTexture::prepareImage(decoded), source), 5);
        images[3] = images[0]; // A clear global image still wins over backups.
        QCOMPARE(radarImageProviders(composeRegionalRadar(images, QSize(2, 2)), source), 8);
        QCOMPARE(radarImageProviders(images[0], source), 0); // No invented legacy provenance.
        QCOMPARE(requestedRadarProviders(WeatherRadarSource()), 1);
    }

    void legendsFollowDisplayedSourcesAndCoverageNeedsNoNetwork()
    {
        WeatherRadarLegend legend;
        legend.setProviders(5);
        QVERIFY(legend.isHidden()); // Provider arrivals cannot override the opt-in.
        legend.setLegendVisible(true);
        QVERIFY(!legend.isHidden());
        auto* noaa = legend.findChild<QLabel*>(QStringLiteral("pskReporterRadarSourceLabel0"));
        auto* canada = legend.findChild<QLabel*>(QStringLiteral("pskReporterRadarSourceLabel1"));
        auto* opera = legend.findChild<QLabel*>(QStringLiteral("pskReporterRadarSourceLabel2"));
        auto* global = legend.findChild<QLabel*>(QStringLiteral("pskReporterRadarSourceLabel3"));
        QVERIFY(noaa && canada && opera && global);
        QVERIFY(!noaa->parentWidget()->isHidden());
        QVERIFY(canada->parentWidget()->isHidden());
        QVERIFY(!opera->parentWidget()->isHidden());
        QVERIFY(global->parentWidget()->isHidden());
        QVERIFY(noaa->text().contains(QStringLiteral("dBZ")));
        QVERIFY(canada->text().contains(QStringLiteral("mm/h")));
        legend.setProviders(8);
        QVERIFY(noaa->parentWidget()->isHidden());
        QVERIFY(!global->parentWidget()->isHidden());
        legend.setLegendVisible(false);
        legend.setProviders(7);
        QVERIFY(legend.isHidden());
        legend.setLegendVisible(true);
        QVERIFY(!legend.isHidden());
        legend.setProviders(0);
        QVERIFY(legend.isHidden());
        MapDisplayWidget map;
        map.setRadarCoverageVisible(true);
        QVERIFY(map.m_weatherRadar->m_radarSites.size() == 389);
        const int count = map.m_weatherRadar->m_radarSites.size();
        map.setRadarCoverageVisible(false);
        map.setRadarCoverageVisible(true);
        QCOMPARE(map.m_weatherRadar->m_radarSites.size(), count);
        QSignalSpy changes(map.m_weatherRadar, &WeatherRadarController::displayedProvidersChanged);
        map.m_weatherRadar->m_weatherRadarVisible = true;
        map.m_weatherRadar->m_flatDisplayedProviders = 5;
        map.m_weatherRadar->publishDisplayedProviders();
        map.m_weatherRadar->publishDisplayedProviders();
        QCOMPARE(changes.size(), 1); // Repeated frames do not relayout the legend.
        map.m_weatherRadar->m_flatDisplayedProviders = 8;
        map.m_weatherRadar->publishDisplayedProviders();
        QCOMPARE(changes.size(), 2);
    }

    void legendPositionTracksMapAndSourceSize()
    {
        MapDisplayWidget map;
        map.resize(800, 600);
        map.show();
        map.m_radarLegend->setProviders(7);
        QVERIFY(map.m_radarLegend->isHidden());
        map.setRadarLegendVisible(true);
        QTRY_COMPARE(map.m_radarLegend->y(), map.height() - map.m_radarLegend->height() - 12);
        QCOMPARE(map.m_radarLegend->x(), 12);
        map.m_radarLegend->setProviders(8);
        QTRY_COMPARE(map.m_radarLegend->y(), map.height() - map.m_radarLegend->height() - 12);
        map.resize(900, 700);
        QTRY_COMPARE(map.m_radarLegend->y(), map.height() - map.m_radarLegend->height() - 12);
        map.setRadarLegendAtTop(true);
        QCOMPARE(map.m_radarLegend->pos(), QPoint(12, 12));
        map.m_radarLegend->setProviders(7);
        QTRY_COMPARE(map.m_radarLegend->pos(), QPoint(12, 12));
        map.setRadarLegendVisible(false);
        map.setRadarLegendAtTop(false);
        map.setRadarLegendVisible(true);
        QTRY_COMPARE(map.m_radarLegend->y(), map.height() - map.m_radarLegend->height() - 12);
        map.m_radarLegend->setProviders(0);
        QVERIFY(map.m_radarLegend->isHidden());
    }

    void compositeObservationNeverUsesFutureOrStaleWeather()
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const QVector<WeatherRadarObservation> observations{{now.addSecs(-300),now.addSecs(-300),{42}},
            {now.addSecs(60),now.addSecs(60),{43}}};
        const auto chosen = radarObservationAt(observations,now);
        QVERIFY(chosen.has_value());
        QCOMPARE(chosen->rasterIds,QVector<qint64>{42});
        QVERIFY(!radarObservationAt(observations,now.addSecs(-600)));
        QVERIFY(!radarObservationAt(observations,now.addSecs(900)));
    }

    void changingRegionsRetiresPreviousProduct()
    {
        MapDisplayWidget map;
        map.setWeatherRadarRegions(7);
        map.m_weatherRadar->m_weatherRadarTimelineCache = QByteArrayLiteral("old catalog");
        map.m_weatherRadar->m_frames.append(WeatherRadarFrame{QDateTime::currentDateTimeUtc()});
        map.setWeatherRadarRegions(4);
        QCOMPARE(map.m_weatherRadar->m_weatherRadarSource.enabledProviders(),4);
        QVERIFY(map.m_weatherRadar->m_frames.isEmpty());
        QVERIFY(map.m_weatherRadar->m_weatherRadarTimelineCache.isEmpty());
        QVERIFY(WeatherRadarSource::composite(7).frameId() != WeatherRadarSource::composite(4).frameId());
        QCOMPARE(map.m_weatherRadar->m_weatherRadarSource.latestFrame().enabledProviders(),4);
        QCOMPARE(map.m_weatherRadar->m_weatherRadarSource.historicalFrame(QDateTime::currentDateTimeUtc()).enabledProviders(),4);
    }

    void noRegionsReturnsTransparentPixelsWithoutNetwork()
    {
        installOperaRadarNetwork();
        MapProviderNetworkAccessManager network;
        const WeatherRadarSource source = WeatherRadarSource::composite(0);
        QNetworkReply* reply = network.get(QNetworkRequest(source.tileUrl(0,0,0)));
        QSignalSpy finished(reply,&QNetworkReply::finished);
        QTRY_COMPARE(finished.size(),1);
        QCOMPARE(reply->error(),QNetworkReply::NoError);
        const QImage image = QImage::fromData(reply->readAll());
        QCOMPARE(image.size(),QSize(512,512));
        QCOMPARE(image.pixelColor(128,128).alpha(),0);
        reply->deleteLater();
    }

    void cityLightsGlobeDrawsAboveDetailAndBelowRadar()
    {
        if (!qEnvironmentVariableIsSet("AETHERSDR_TEST_RADAR_GL")) {
            QSKIP("Opt in with AETHERSDR_TEST_RADAR_GL=1 and a native GUI platform");
        }
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        GlobeMapView globe;
        globe.resize(600, 400);
        globe.show();
        QTRY_VERIFY(globe.isValid());
        globe.cancelTileRequests();
        globe.m_navigation.reset(31, -79);
        globe.m_cameraDistance = 1.2F;
        globe.m_detailSelectionDirty = false;
        globe.m_atlas.fill(Qt::black);
        globe.m_atlasDirty = true;
        globe.makeCurrent();
        globe.m_visibleDetailKeys.clear();
        globe.m_detailTiles.clear();
        for (int y = 50; y <= 54; ++y) {
            for (int x = 34; x <= 37; ++x) {
                auto tile = std::make_shared<GlobeMapView::DetailTile>();
                tile->zoom = 7;
                tile->x = x;
                tile->y = y;
                tile->image = QImage(256, 256, QImage::Format_RGBA8888);
                tile->image.fill(Qt::black);
                globe.uploadDetailTile(*tile);
                const QString key = globe.detailTileKey(7, x, y);
                globe.m_detailTiles.insert(key, tile);
                globe.m_visibleDetailKeys.append(key);
            }
        }
        globe.doneCurrent();
        const QRectF world(-kRadarMercatorExtent, -kRadarMercatorExtent,
                           kRadarWorldWidth, kRadarWorldWidth);
        QImage lights(256, 256, QImage::Format_ARGB32_Premultiplied);
        lights.fill(Qt::green);
        globe.setDayNightTerminatorVisible(false);
        globe.setCityLightsImage(lights, world);
        globe.setCityLightsBrightness(100);
        globe.setCityLightsVisible(true);
        const auto center = [&globe] {
            const QImage frame = globe.grabFramebuffer();
            return frame.pixelColor(frame.width() / 2, frame.height() / 2);
        };
        QTRY_VERIFY(center().green() > 240);
        globe.setCityLightsBrightness(50);
        QTRY_VERIFY(center().green() > 110 && center().green() < 145);
        globe.setCityLightsVisible(false);
        QTRY_VERIFY(center().green() < 10);
        globe.setCityLightsVisible(true);
        globe.setCityLightsBrightness(100);
        QTRY_VERIFY(center().green() > 240);
        lights.fill(qRgba(16, 8, 0, 16));
        globe.setCityLightsImage(lights, world);
        globe.setCityLightsFaintLights(0);
        QTRY_VERIFY(center().red() >= 14 && center().red() <= 18);
        const auto* resident = globe.m_cityLightsTexture.get();
        const qint64 imageKey = globe.m_cityLightsImage.cacheKey();
        for (int value : {50, 100, 0, 80}) {
            globe.setCityLightsFaintLights(value);
            QVERIFY(!globe.m_cityLightsDirty);
            const int expected = qRound(255 * std::pow(16.0 / 255, 1.0 - 0.65 * value / 100));
            QTRY_VERIFY(std::abs(center().red() - expected) <= 3);
            QCOMPARE(globe.m_cityLightsTexture.get(), resident);
            QCOMPARE(globe.m_cityLightsImage.cacheKey(), imageKey);
        }
        const auto* lightsProgram = globe.m_cityLightsProgram.get();
        globe.makeCurrent();
        globe.uploadAtlas();
        const bool retainedTexture = globe.m_cityLightsTexture.get() == resident;
        const bool retainedProgram = globe.m_cityLightsProgram.get() == lightsProgram;
        globe.doneCurrent();
        QVERIFY2(retainedTexture && retainedProgram,
                 "Basemap uploads must preserve city-light GPU resources");
        const int enhanced = qRound(255 * std::pow(16.0 / 255, 1.0 - 0.65 * 0.8));
        QTRY_VERIFY(std::abs(center().red() - enhanced) <= 3);
        lights.fill(Qt::white);
        globe.setCityLightsImage(lights, world);
        globe.setCityLightsWarmth(0);
        QTRY_VERIFY(center().blue() > 250);
        const auto* warmTexture = globe.m_cityLightsTexture.get();
        globe.setCityLightsWarmth(25);
        QTRY_VERIFY(std::abs(center().blue() - 230) <= 3);
        QVERIFY(std::abs(center().green() - 245) <= 3);
        QCOMPARE(globe.m_cityLightsTexture.get(), warmTexture);
        QVERIFY(!globe.m_cityLightsDirty);
        globe.m_weatherRadarVisible = true;
        QImage rain(256, 256, WeatherRadarTexture::kImageFormat);
        rain.fill(Qt::blue);
        const QDateTime observation = QDateTime::currentDateTimeUtc();
        QTRY_VERIFY(globe.showWeatherRadarPlaybackFrame(rain, observation, world));
        QTRY_VERIFY(center().blue() > 180 && center().green() < 80);
        QGV::setNetworkManager(nullptr);
    }

    void cityLightsFlatWrapAndOpacity()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        MapView map(nullptr, MapView::ViewportMode::Raster);
        map.resize(600, 400);
        map.show();
        QGVMap* flat = map.findChild<QGVMap*>();
        QImage lights(8, 8, QImage::Format_ARGB32_Premultiplied);
        lights.fill(Qt::green);
        const QRectF region(-1.1e7, 3.5e6, 1e6, 1e6);
        map.setCityLightsImage(lights, region);
        map.setCityLightsBrightness(100);
        map.setCityLightsVisible(true);
        const auto centerGreen = [&flat] {
            const QImage frame = flat->geoView()->viewport()->grab().toImage();
            return frame.pixelColor(frame.width() / 2, frame.height() / 2).green();
        };
        for (int copy : {-3, 0, 3}) {
            flat->cameraTo(QGVCameraActions(flat).scaleTo(.001)
                .moveTo(QPointF(region.center().x() + copy * kRadarWorldWidth,
                               -region.center().y())), false);
            QTRY_VERIFY(centerGreen() > 240);
        }
        map.setCityLightsBrightness(0);
        QTRY_VERIFY(centerGreen() < 200);
        map.setCityLightsBrightness(100);
        QTRY_VERIFY(centerGreen() > 240);
        map.setCityLightsVisible(false);
        QTRY_VERIFY(centerGreen() < 200);
        QGV::setNetworkManager(nullptr);
    }

    void partialHistoryRetriesMissingOriginal_data()
    {
        QTest::addColumn<int>("failedIndex");
        QTest::newRow("oldest") << 0;
        QTest::newRow("interior") << 2;
    }

    void partialHistoryRetriesMissingOriginal()
    {
        QFETCH(int, failedIndex);
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        MapDisplayWidget map;
        prepare(map, network);
        const QVector<QDateTime> originalFrames = map.m_weatherRadar->frameValues(&WeatherRadarFrame::time);
        QVector<WeatherRadarObservation> catalog;
        for (int i = 0; i < originalFrames.size(); ++i) {
            catalog.append({originalFrames.at(i), map.m_weatherRadar->m_frames.at(i).sampleTime,
                            map.m_weatherRadar->m_frames.at(i).rasterIds});
        }
        network.pending(map.m_weatherRadar->m_frames.at(failedIndex).url)->complete(true);
        finishAll(map, network);
        map.m_weatherRadar->m_weatherRadarPlaybackTimer->stop();
        map.m_weatherRadar->applyFinalizedWeatherRadarBuffering();
        QCOMPARE(map.m_weatherRadar->m_frames.size(), 5);
        QVERIFY(map.m_weatherRadar->m_weatherRadarRebufferTimer->isActive());
        const QVector<QDateTime> activeFrames = map.m_weatherRadar->frameValues(&WeatherRadarFrame::time);
        const QVector<int> activeDurations = map.m_weatherRadar->m_weatherRadarActiveSegmentDurationsMs;
        const int requests = network.requests.size();
        map.m_weatherRadar->rebufferWeatherRadarPlayback();
        QVERIFY(map.m_weatherRadar->m_weatherRadarTimelineReply);
        map.m_weatherRadar->cancelWeatherRadarTimelineRequest();
        map.m_weatherRadar->appendWeatherRadarObservations(catalog);
        QCOMPARE(network.requests.size(), requests + 1); // Only the missing image.
        QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::time).mid(0, 5), activeFrames);
        QCOMPARE(map.m_weatherRadar->m_weatherRadarActiveSegmentDurationsMs, activeDurations);
        QCOMPARE(map.m_weatherRadar->weatherRadarPlaybackFrameCount(), 5);
        // A view refresh while the older retry is staged must not reject its
        // temporarily unsorted tail or change the active movie's cadence.
        map.m_weatherRadar->cancelWeatherRadarFrameRequests();
        map.m_weatherRadar->bufferWeatherRadarFrames(true);
        QVERIFY(map.m_weatherRadar->m_weatherRadarAnimating);
        QCOMPARE(map.m_weatherRadar->m_weatherRadarActiveSegmentDurationsMs, activeDurations);
        finishAll(map, network);
        QVERIFY(map.m_weatherRadar->m_weatherRadarBufferFinalizationPending);
        QCOMPARE(map.m_weatherRadar->weatherRadarPlaybackFrameCount(), 5);
        map.m_weatherRadar->applyFinalizedWeatherRadarBuffering(); // The loop-restart operation.
        QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::time), originalFrames);
        QCOMPARE(map.m_weatherRadar->weatherRadarPlaybackFrameCount(), 6);
        QVERIFY(map.m_weatherRadar->m_weatherRadarRetryFrames.isEmpty());
        QVERIFY(map.m_weatherRadar->m_weatherRadarDownloadFailed.isEmpty());
        map.stopWeatherRadarAnimation();
        QGV::setNetworkManager(nullptr);
    }

    void completedLiveTilesSurviveSmallPan()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        QGVMap map;
        map.resize(600, 400);
        map.show();
        QTest::qWait(20);
        map.cameraTo(QGVCameraActions(&map).scaleTo(.0001)
            .moveTo(QPointF(-1.05e7, -4.0e6)), false);
        QCoreApplication::processEvents();
        auto* layer = new WeatherRadarTileLayer();
        layer->setEnabled(false);
        map.addItem(layer);
        QSignalSpy ready(layer, &WeatherRadarTileLayer::frameReady);
        layer->setEnabled(true);
        QTRY_VERIFY(!network.allReplies.isEmpty());
        QTest::qWait(200);
        const int requests = network.allReplies.size();
        for (const auto& reply : std::as_const(network.allReplies)) {
            reply->complete();
        }
        // Two screen pixels: a genuine camera change with the same tile set.
        // Deliver all replies, then pan before the 25ms readiness tick.
        map.cameraTo(QGVCameraActions(&map)
            .moveTo(QPointF(-1.05e7 + 20000, -4.0e6)), false);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 1500);
        QCOMPARE(network.allReplies.size(), requests);
        QCOMPARE(layer->pendingRequestCount(), 0);
        // A second pan after readiness must also acknowledge existing coverage.
        map.cameraTo(QGVCameraActions(&map)
            .moveTo(QPointF(-1.05e7 + 40000, -4.0e6)), false);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 2, 1500);
        layer->setEnabled(false);
        QGV::setNetworkManager(nullptr);
    }

    void liveTilesRetryWithoutCameraMovement()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        QGVMap map;
        map.resize(600, 400);
        map.show();
        QTest::qWait(20);
        map.cameraTo(QGVCameraActions(&map).scaleTo(.0001)
            .moveTo(QPointF(-1.05e7, -4.0e6)), false);
        QCoreApplication::processEvents();
        auto* layer = new WeatherRadarTileLayer();
        layer->setEnabled(false);
        map.addItem(layer);
        QSignalSpy ready(layer, &WeatherRadarTileLayer::frameReady);
        layer->setEnabled(true);
        QTRY_VERIFY(!network.allReplies.isEmpty());
        QTest::qWait(200);
        const int requests = network.allReplies.size();
        const auto replies = network.allReplies;
        // One missing tile; all other completed tiles must remain in place.
        for (int i = 0; i < replies.size(); ++i) {
            replies.at(i)->complete(i == 0);
        }
        layer->checkReadiness(999, 999);
        QCOMPARE(network.allReplies.size(), requests);
        layer->checkReadiness(5000, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(network.allReplies.size(), requests + 1, 3000);
        QVERIFY(ready.isEmpty());
        network.allReplies.last()->complete();
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(!layer->loadFailed());
        QCOMPARE(layer->pendingRequestCount(), 0);
        layer->setEnabled(false);
        QGV::setNetworkManager(nullptr);
    }

    void liveTilesRetryDuringCameraMovement()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        QGVMap map;
        map.resize(600, 400);
        map.show();
        QTest::qWait(20);
        map.cameraTo(QGVCameraActions(&map).scaleTo(.0001)
            .moveTo(QPointF(-1.05e7, -4.0e6)), false);
        QCoreApplication::processEvents();
        auto* layer = new WeatherRadarTileLayer();
        layer->setEnabled(false);
        map.addItem(layer);
        QSignalSpy ready(layer, &WeatherRadarTileLayer::frameReady);
        layer->setEnabled(true);
        QTRY_VERIFY(!network.allReplies.isEmpty());
        QTest::qWait(200);
        const int requests = network.allReplies.size();
        const auto replies = network.allReplies;
        for (int i = 0; i < replies.size(); ++i) {
            replies.at(i)->complete(i == 0);
        }
        // Real camera events repeatedly reset the readiness settle clock, but
        // must not reset the independent retry clock. Stay within the same tiles.
        for (int i = 0; i < 12; ++i) {
            map.cameraTo(QGVCameraActions(&map)
                .moveTo(QPointF(-1.05e7 + (i % 2) * 10000, -4.0e6)), false);
            QTest::qWait(150);
        }
        QCOMPARE(network.allReplies.size(), requests + 1);
        network.allReplies.last()->complete();
        QTRY_COMPARE(ready.size(), 1);
        layer->setEnabled(false);
        QGV::setNetworkManager(nullptr);
    }

    void completedCoverageDoesNotWaitForOldFailures()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        QGVMap map;
        map.resize(600, 400);
        map.show();
        QTest::qWait(20);
        map.cameraTo(QGVCameraActions(&map).scaleTo(.0001)
            .moveTo(QPointF(-1.05e7, -4.0e6)), false);
        QCoreApplication::processEvents();
        auto* layer = new WeatherRadarTileLayer();
        layer->setEnabled(false);
        map.addItem(layer);
        QSignalSpy ready(layer, &WeatherRadarTileLayer::frameReady);
        layer->setEnabled(true);
        QTRY_VERIFY(!network.allReplies.isEmpty());
        QTest::qWait(200);
        const int requests = network.allReplies.size();
        const auto replies = network.allReplies;
        for (int i = 0; i < replies.size(); ++i) {
            replies.at(i)->complete(i == 0);
        }
        // Recover the tile outside the readiness callback, leaving its old
        // failure count intact. Completed current coverage must win immediately.
        layer->retryUnfinishedTiles();
        QCOMPARE(network.allReplies.size(), requests + 1);
        network.allReplies.last()->complete();
        QVERIFY(layer->currentTilesComplete());
        layer->checkReadiness(300, 0);
        QCOMPARE(ready.size(), 1);
        layer->setEnabled(false);
        QGV::setNetworkManager(nullptr);
    }

    void liveRadarAtCloseZoom_data()
    {
        QTest::addColumn<double>("scale");
        QTest::newRow("zoom12-control") << 0.03125;
        QTest::newRow("zoom13") << 0.0625;
        QTest::newRow("zoom17") << 1.0;
    }

    void liveTileFailureReportsOnceAndRecovers()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        QGVMap map;
        map.resize(600, 400);
        map.show();
        QTest::qWait(20);
        map.cameraTo(QGVCameraActions(&map).scaleTo(.0001)
            .moveTo(QPointF(-1.05e7, -4.0e6)), false);
        QCoreApplication::processEvents();
        auto* layer = new WeatherRadarTileLayer();
        layer->setEnabled(false);
        map.addItem(layer);
        QSignalSpy failures(layer, &WeatherRadarTileLayer::frameLoadFailed);
        QSignalSpy ready(layer, &WeatherRadarTileLayer::frameReady);
        layer->setEnabled(true);
        QElapsedTimer deadline;
        deadline.start();
        while (failures.isEmpty() && deadline.elapsed() < 6000) {
            const auto replies = network.allReplies;
            for (const auto& reply : replies) {
                if (reply && !reply->isFinished()) {
                    reply->complete(true);
                }
            }
            layer->checkReadiness(5000, 5000); // Advance retry time without sleeping.
            QTest::qWait(25);
        }
        QCOMPARE(failures.size(), 1);
        QVERIFY(layer->loadFailed());
        QVERIFY(layer->isVisible());
        QVERIFY(layer->pendingRequestCount() > 0);
        const auto replies = network.allReplies;
        for (const auto& reply : replies) {
            if (reply && !reply->isFinished()) {
                reply->complete();
            }
        }
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(!layer->loadFailed());
        QCOMPARE(failures.size(), 1);
        layer->setEnabled(false);
        const int count = network.allReplies.size();
        QTest::qWait(100);
        QCOMPARE(network.allReplies.size(), count);
        QGV::setNetworkManager(nullptr);
    }

    void liveRadarAtCloseZoom()
    {
        QFETCH(double, scale);
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        QGVMap map;
        map.resize(600, 400);
        map.show();
        QTest::qWait(20);
        map.cameraTo(QGVCameraActions(&map).scaleTo(scale)
            .moveTo(QPointF(-1.05e7, -4.0e6)), false);
        QCoreApplication::processEvents();
        auto* layer = new WeatherRadarTileLayer();
        layer->setEnabled(false);
        map.addItem(layer);
        layer->setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(!network.allReplies.isEmpty(), 1000);
        QCOMPARE(map.getCamera().scale(), scale); // Never limit the map camera.
        const int initialRequests = network.allReplies.size();
        for (const auto& reply : std::as_const(network.allReplies)) {
            reply->complete();
        }
        layer->setSource(WeatherRadarSource(WeatherRadarSource::Provider::NoaaMrms,
            QDateTime::currentDateTimeUtc().addSecs(300)));
        QTRY_VERIFY(network.allReplies.size() > initialRequests); // Refresh also works.
        layer->setEnabled(false);
        QGV::setNetworkManager(nullptr);
    }

    void catalogRetiresExpiredFramesWithoutANewObservation()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        MapDisplayWidget map;
        prepare(map, network);
        finishAll(map, network);
        map.m_weatherRadar->applyFinalizedWeatherRadarBuffering();
        QCOMPARE(map.m_weatherRadar->weatherRadarPlaybackFrameCount(), 6);
        map.m_weatherRadar->m_weatherRadarPlaybackTimer->stop();
        const auto frames = map.m_weatherRadar->frameValues(&WeatherRadarFrame::time);
        const auto bytes = map.m_weatherRadar->frameValues(&WeatherRadarFrame::bytes);
        const int downloads = network.requests.size();
        QVector<WeatherRadarObservation> catalog;
        for (int i = 1; i < frames.size(); ++i) {
            catalog.append({frames.at(i), map.m_weatherRadar->m_frames.at(i).sampleTime,
                            map.m_weatherRadar->m_frames.at(i).rasterIds});
        }
        map.m_weatherRadar->appendWeatherRadarObservations(catalog);
        QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::time), frames); // Do not renumber mid-loop.
        QVERIFY(map.m_weatherRadar->m_weatherRadarBufferFinalizationPending);
        map.m_weatherRadar->applyFinalizedWeatherRadarBuffering();
        QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::time), frames.mid(1));
        QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::bytes), bytes.mid(1));
        QCOMPARE(network.requests.size(), downloads);
        map.stopWeatherRadarAnimation();
        QGV::setNetworkManager(nullptr);
    }

    void emptyExportRequiresAvailableRasters_data()
    {
        QTest::addColumn<bool>("globe");
        QTest::addColumn<int>("result"); // 0 expired, 1 clear weather, 2 offline, 3 canceled.
        for (bool globe : {false, true}) {
            for (int result = 0; result < 4; ++result) {
                QTest::newRow(qPrintable(QString("%1-%2").arg(globe ? "globe" : "flat").arg(result)))
                    << globe << result;
            }
        }
    }

    void emptyExportRequiresAvailableRasters()
    {
        QFETCH(bool, globe);
        QFETCH(int, result);
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        MapDisplayWidget map;
        prepare(map, network);
        finishAll(map, network);
        map.m_weatherRadar->applyFinalizedWeatherRadarBuffering();
        QCOMPARE(map.m_weatherRadar->weatherRadarPlaybackFrameCount(), 6);
        map.m_weatherRadar->m_weatherRadarPlaybackTimer->stop();
        if (globe) {
            map.hide();
            map.setProjectionMode(MapDisplayWidget::ProjectionMode::Globe);
        }
        map.m_weatherRadar->m_weatherRadarRebufferTimer->stop();
        const auto frames = map.m_weatherRadar->frameValues(&WeatherRadarFrame::time);
        const auto bytes = map.m_weatherRadar->frameValues(&WeatherRadarFrame::bytes);
        // Reproduce NOAA expiring an immutable raster BETWEEN catalog fetch
        // and a zoom export: HTTP 200, correctly sized PNG, every alpha zero.
        map.m_weatherRadar->m_weatherRadarFrameCache.clear();
        map.m_weatherRadar->cancelWeatherRadarFrameRequests();
        map.m_weatherRadar->m_weatherRadarDetailRefresh = true;
        map.m_weatherRadar->m_weatherRadarRequestedView = {map.m_weatherRadar->weatherRadarCurrentView().bounds, QSize(256, 256)};
        map.m_weatherRadar->m_frames[0].cacheKey.clear();
        map.m_weatherRadar->m_frames[0].url = WeatherRadarSource::historicalNoaaFrame(
            frames.first(), map.m_weatherRadar->m_frames.first().sampleTime, {100}).imageUrl(
                map.m_weatherRadar->m_weatherRadarRequestedView.bounds, map.m_weatherRadar->m_weatherRadarRequestedView.size);
        // Feed the same production decode path as the network reply.
        QImage clear(map.m_weatherRadar->m_weatherRadarRequestedView.size, WeatherRadarTexture::kImageFormat);
        clear.fill(Qt::transparent);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        clear.save(&buffer, "PNG");
        const QString key = map.m_weatherRadar->m_frames.first().url.toString(QUrl::FullyEncoded);
        map.m_weatherRadar->m_weatherRadarNetworkRequestsComplete = false;
        map.m_weatherRadar->decodeWeatherRadarDownload(0, png, key, map.m_weatherRadar->m_weatherRadarRequestedView);
        QTRY_VERIFY(network.pendingValidation() != nullptr);
        QCOMPARE(map.m_weatherRadar->m_frames.first().bytes, bytes.first());
        if (result == 3) {
            const QPointer<ControlledRadarReply> verification = network.pendingValidation();
            map.stopWeatherRadarAnimation();
            // A late reply from a stopped/changed generation cannot resurrect
            // playback or install its unverified empty pixels into the cache.
            if (verification) {
                verification->completeJson(R"({"objectIds":[100]})");
            }
            QCoreApplication::processEvents();
            QVERIFY(!map.m_weatherRadar->m_weatherRadarPlaybackRequested);
            QVERIFY(map.m_weatherRadar->m_frames.isEmpty());
            QVERIFY(!map.m_weatherRadar->m_weatherRadarFrameCache.contains(key));
            QGV::setNetworkManager(nullptr);
            return;
        }
        if (result == 2) {
            network.pendingValidation()->complete(true);
        } else {
            network.pendingValidation()->completeJson(result == 1
                ? R"({"objectIds":[100]})" : R"({"objectIds":[]})");
        }
        QTRY_VERIFY(!map.m_weatherRadar->m_weatherRadarDownloadDecodePending.contains(0));
        map.m_weatherRadar->updateWeatherRadarLoadingStatus();
        if (result == 0) {
            // Confirmed source expiration is normal rolling-history upkeep,
            // not a download failure and not a reason to retry that export.
            QVERIFY(!map.m_weatherRadar->m_weatherRadarDownloadFailed.contains(0));
            QVERIFY(!map.m_weatherRadar->m_weatherRadarRebufferTimer->isActive());
            QVERIFY(map.m_weatherRadarLoadingLabel->text() != QStringLiteral("Loading radar data failed"));
        } else if (result == 2) {
            QCOMPARE(map.m_weatherRadarLoadingLabel->text(), QStringLiteral("Loading radar data failed"));
            QVERIFY(map.m_weatherRadar->m_weatherRadarRebufferTimer->isActive());
        }
        if (result == 1) {
            QCOMPARE(map.m_weatherRadar->m_frames.first().bytes, png);
            QVERIFY(map.m_weatherRadar->m_weatherRadarFrameCache.contains(key));
            // Trusted clear originals may be replayed from compressed cache
            // without another network verification on every loop.
            map.m_weatherRadar->m_weatherRadarFrameCache[key].decodedImage = {};
            map.m_weatherRadar->m_weatherRadarBufferQueue = {0};
            map.m_weatherRadar->requestNextWeatherRadarBufferedFrames();
            QTRY_VERIFY(!map.m_weatherRadar->m_weatherRadarDownloadDecodePending.contains(0));
            QVERIFY(network.pendingValidation() == nullptr);
        } else {
            QCOMPARE(map.m_weatherRadar->m_frames.first().bytes, bytes.first());
            QVERIFY(!map.m_weatherRadar->m_weatherRadarFrameCache.contains(key));
            map.m_weatherRadar->m_weatherRadarRebufferTimer->stop();
            if (result == 0) {
                QVERIFY(map.m_weatherRadar->m_weatherRadarBufferFinalizationPending);
                map.m_weatherRadar->applyFinalizedWeatherRadarBuffering();
                QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::time), frames.mid(1));
                QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::bytes), bytes.mid(1));
            } else {
                QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::time), frames);
                QVERIFY(map.m_weatherRadar->m_weatherRadarDownloadFailed.contains(0));
            }
        }
        map.stopWeatherRadarAnimation();
        QGV::setNetworkManager(nullptr);
    }

    void flatControllerLoopRetainsEveryPaint()
    {
#if defined(Q_OS_MAC) && defined(AETHER_GPU_SPECTRUM)
        // MapDisplayWidget::flatMapViewportMode() deliberately keeps the flat
        // map on QGraphicsView's raster viewport in this configuration, so
        // m_flatView owns no QOpenGLWidget for this case to sample. The target
        // as registered does not define AETHER_GPU_SPECTRUM, so this only
        // fires for someone compiling the map sources the way the app is
        // compiled. Without it, that build reads as a regression rather than
        // as the raster path the app deliberately selects.
        QSKIP("macOS GPU-spectrum builds keep the flat map on the raster "
              "viewport; there is no QOpenGLWidget to read pixels from");
#endif
        if (!qEnvironmentVariableIsSet("AETHERSDR_TEST_RADAR_GL")) {
            QSKIP("Opt in with AETHERSDR_TEST_RADAR_GL=1 and a native GUI platform");
        }
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        MapDisplayWidget map;
        map.m_weatherRadar->m_weatherRadarHistoryHours = 4;
        prepare(map, network, 18, true);
        QOpenGLWidget* viewport = map.m_flatView->findChild<QOpenGLWidget*>();
        QVERIFY(viewport != nullptr);
        QTRY_VERIFY(viewport->isValid());
        map.setWeatherRadarPlaybackSpeed(500);
        QElapsedTimer deadline;
        deadline.start();
        while (!map.m_weatherRadar->m_weatherRadarNetworkRequestsComplete && deadline.elapsed() < 5000) {
            if (ControlledRadarReply* reply = network.pending()) {
                reply->complete(false, Qt::green);
            }
            QTest::qWait(10);
        }
        QTRY_COMPARE(map.m_weatherRadar->weatherRadarPlaybackFrameCount(), 18);
        QTRY_VERIFY(map.m_weatherRadar->m_weatherRadarPresentedImageKey != 0);
        map.m_weatherRadar->m_weatherRadarFrameCache.clear();
        int paints = 0;
        int blanks = 0;
        int restarts = 0;
        int previousIndex = -1;
        const QMetaObject::Connection capture = connect(
            viewport, &QOpenGLWidget::aboutToCompose, &map, [&] {
                viewport->makeCurrent();
                QOpenGLFunctions* gl = viewport->context()->functions();
                gl->glBindFramebuffer(GL_FRAMEBUFFER, viewport->defaultFramebufferObject());
                GLubyte rgba[4]{};
                gl->glReadPixels(qRound(viewport->width() * viewport->devicePixelRatioF() / 2),
                    qRound(viewport->height() * viewport->devicePixelRatioF() / 2), 1, 1,
                    GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                ++paints;
                if (rgba[1] < 150) {
                    ++blanks;
                }
                if (map.m_weatherRadar->m_weatherRadarFrameIndex == 0 && previousIndex > 0) {
                    ++restarts;
                }
                previousIndex = map.m_weatherRadar->m_weatherRadarFrameIndex;
            });
        QTRY_VERIFY_WITH_TIMEOUT(restarts >= 4, 20000);
        disconnect(capture);
        QVERIFY(paints > 30);
        QCOMPARE(blanks, 0);
        map.stopWeatherRadarAnimation();
        QGV::setNetworkManager(nullptr);
    }

    void globeControllerLoopRetainsEveryPaint()
    {
        if (!qEnvironmentVariableIsSet("AETHERSDR_TEST_RADAR_GL")) {
            QSKIP("Opt in with AETHERSDR_TEST_RADAR_GL=1 and a native GUI platform");
        }
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        MapDisplayWidget map;
        map.setProjectionMode(MapDisplayWidget::ProjectionMode::Globe);
        map.m_weatherRadar->m_weatherRadarHistoryHours = 4;
        prepare(map, network, 18);
        GlobeMapView& globe = *map.m_globeView;
        globe.m_navigation.reset(35, -80);
        globe.m_cameraDistance = 1.2F;
        globe.m_detailSelectionDirty = false;
        globe.m_weatherRadarVisible = true;
        globe.m_terminatorVisible = false;
        map.setWeatherRadarPlaybackSpeed(500);
        QTRY_VERIFY(globe.isValid());
        QElapsedTimer deadline;
        deadline.start();
        while (!map.m_weatherRadar->m_weatherRadarNetworkRequestsComplete && deadline.elapsed() < 5000) {
            if (auto* reply = network.pending()) {
                reply->complete(false, Qt::green);
            }
            QTest::qWait(10);
        }
        QTRY_COMPARE(map.m_weatherRadar->weatherRadarPlaybackFrameCount(), 18);
        QTRY_VERIFY(map.m_weatherRadar->m_weatherRadarPresentedImageKey != 0);
        // Exercise real PNG re-decodes/new QImage identities on every wrap,
        // not only six images held forever by the cache or the test itself.
        map.m_weatherRadar->m_weatherRadarFrameCache.clear();
        int paints = 0;
        int blanks = 0;
        int restarts = 0;
        int previousIndex = -1;
        const auto capture = connect(&globe, &QOpenGLWidget::aboutToCompose, &map, [&] {
            globe.makeCurrent();
            QOpenGLFunctions* gl = globe.context()->functions();
            gl->glBindFramebuffer(GL_FRAMEBUFFER, globe.defaultFramebufferObject());
            GLubyte rgba[4]{};
            gl->glReadPixels(qRound(globe.width() * globe.devicePixelRatioF() / 2),
                qRound(globe.height() * globe.devicePixelRatioF() / 2), 1, 1,
                GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            ++paints;
            if (rgba[1] < 150) {
                ++blanks;
                qWarning() << "Blank controller paint" << map.m_weatherRadar->m_weatherRadarFrameIndex
                    << globe.m_radarTextureFrameTime << int(rgba[0]) << int(rgba[1]) << int(rgba[2]);
            }
            if (map.m_weatherRadar->m_weatherRadarFrameIndex == 0 && previousIndex > 0) {
                ++restarts;
            }
            previousIndex = map.m_weatherRadar->m_weatherRadarFrameIndex;
        });
        QTRY_VERIFY_WITH_TIMEOUT(restarts >= 4, 20000);
        disconnect(capture);
        QVERIFY(paints > 30);
        QCOMPARE(blanks, 0);
        map.stopWeatherRadarAnimation();
        QGV::setNetworkManager(nullptr);
    }

    void conciseLoadingLifecycle()
    {
        using State = WeatherRadarLoadingStatus::State;
        WeatherRadarLoadingStatus status;
        QCOMPARE(status.update(0, true, false, 18, 0), State::Hidden);
        QCOMPARE(status.update(299, true, false, 18, 0), State::Hidden);
        QCOMPARE(status.update(300, true, false, 18, 0), State::Loading);
        // A long batch that is progressing must not report a timeout.
        QCOMPARE(status.update(14000, true, false, 17, 1), State::Loading);
        QCOMPARE(status.update(28000, true, false, 16, 2), State::Loading);
        QCOMPARE(status.update(42999, true, false, 16, 2), State::Loading);
        QCOMPARE(status.update(43000, true, false, 16, 2), State::Failed);
        QCOMPARE(status.update(45999, true, true, 16, 2), State::Failed);
        QCOMPARE(status.update(46000, true, true, 16, 2), State::Hidden);
        // Background retries make progress without redisplaying the badge.
        QCOMPARE(status.update(48000, true, false, 4, 14), State::Hidden);
        QCOMPARE(status.update(49000, false, true, 0, 17), State::Hidden);
        QCOMPARE(status.update(50000, false, false, 0, 18), State::Hidden);
        QCOMPARE(status.update(51000, true, false, 18, 0), State::Hidden);
        QCOMPARE(status.update(51300, true, false, 18, 0), State::Loading);
        QCOMPARE(status.update(51400, false, true, 0, 0), State::Failed);
        QCOMPARE(status.update(54400, false, true, 0, 0), State::Hidden);
    }

    void initialFailureRetriesWithoutLosingIntent()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        MapDisplayWidget map;
        map.m_weatherRadar->m_weatherRadarNetwork = &network;
        map.m_weatherRadar->m_weatherRadarVisible = true;
        QSignalSpy errors(&map, &MapDisplayWidget::weatherRadarAnimationError);
        map.startWeatherRadarAnimation(1);
        QVERIFY(map.m_weatherRadar->m_weatherRadarTimelineReply);
        static_cast<ControlledRadarReply*>(map.m_weatherRadar->m_weatherRadarTimelineReply)->complete(true);
        QVERIFY(map.weatherRadarAnimating()); // Includes requested/retrying playback.
        QVERIFY(map.m_weatherRadar->m_weatherRadarTimelineFailed);
        QVERIFY(map.m_weatherRadar->m_weatherRadarRebufferTimer->isActive());
        QVERIFY(errors.isEmpty());
        QCOMPARE(map.m_weatherRadar->m_weatherRadarRebufferTimer->interval(), MapProviderRetryPolicy::kConsumerRetryMs);
        QCOMPARE(map.m_weatherRadar->m_weatherRadarRebufferTimer->timerType(), Qt::PreciseTimer);
        map.m_weatherRadar->m_weatherRadarRebufferTimer->stop();
        map.m_weatherRadar->rebufferWeatherRadarPlayback(); // Same callback as the jitter-aware retry.
        QVERIFY(map.m_weatherRadar->m_weatherRadarTimelineReply);
        QCOMPARE(map.m_weatherRadar->m_weatherRadarTimelineReply->url(), WeatherRadarSource::noaaTimelineUrl());
        map.stopWeatherRadarAnimation();
        QVERIFY(!map.m_weatherRadar->m_weatherRadarRebufferTimer->isActive());
        QVERIFY(!map.m_weatherRadar->m_weatherRadarTimelineReply);
        QVERIFY(!map.weatherRadarAnimating());
        QGV::setNetworkManager(nullptr);
    }

    void speedChangesRetainFramesAndFinalHold()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        MapDisplayWidget map;
        prepare(map, network);
        map.m_flatView->setWeatherRadarVisible(true);
        finishAll(map, network);
        QTRY_COMPARE(map.m_weatherRadar->weatherRadarPlaybackFrameCount(), 6);
        map.m_weatherRadar->m_weatherRadarPlaybackTimer->stop();
        map.m_weatherRadar->m_weatherRadarTimer->stop();
        const auto bytes = map.m_weatherRadar->frameValues(&WeatherRadarFrame::bytes);
        const auto keys = map.m_weatherRadar->frameValues(&WeatherRadarFrame::cacheKey);
        const auto bounds = map.m_weatherRadar->frameValues(&WeatherRadarFrame::bounds);
        const int generation = map.m_weatherRadar->m_weatherRadarBufferGeneration;
        const int requests = network.requests.size();
        for (const int speed : {25, 73, 400, 500, 100}) {
            map.setWeatherRadarPlaybackSpeed(speed);
            map.m_weatherRadar->rebufferWeatherRadarPlayback(); // Even a queued view callback is a no-op.
            QCOMPARE(map.m_weatherRadar->m_weatherRadarPlaybackSpeedPercent, speed);
            QCOMPARE(map.m_weatherRadar->m_weatherRadarBufferGeneration, generation);
            QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::bytes), bytes);
            QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::cacheKey), keys);
            QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::bounds), bounds);
            QCOMPARE(network.requests.size(), requests);

            qint64 lastStart = 0;
            for (const int duration : map.m_weatherRadar->m_weatherRadarActiveSegmentDurationsMs) {
                lastStart += duration;
            }
            // Enter the actual final original, then hold it through many
            // ticks and a speed change. Neither preloading frame zero nor a
            // changed clock is permission to clear the displayed image.
            map.m_weatherRadar->ensureWeatherRadarDecodeAhead(5);
            QTRY_VERIFY(map.m_weatherRadar->m_weatherRadarDecodedImages.contains(5));
            map.m_weatherRadar->m_weatherRadarPlaybackCadence.reset(lastStart,
                map.m_weatherRadar->m_weatherRadarPlaybackClock.elapsed());
            QVERIFY(map.m_weatherRadar->tryPresentWeatherRadarElapsed(lastStart));
            // Seed an acknowledged final frame for the clock-only test. The
            // native framebuffer test separately checks actual retained pixels.
            map.m_weatherRadar->m_weatherRadarPlaybackCadence.rebaseElapsed(lastStart);
            const qint64 imageKey = map.m_weatherRadar->m_weatherRadarPresentedImageKey;
            for (int hold = 16; hold < 1000; hold += 16) {
                QVERIFY(map.m_weatherRadar->tryPresentWeatherRadarElapsed(lastStart + hold));
                QCOMPARE(map.m_weatherRadar->m_weatherRadarFrameIndex, 5);
                QCOMPARE(map.m_weatherRadar->m_weatherRadarPresentedImageKey, imageKey);
            }
            QVERIFY(map.m_weatherRadar->tryPresentWeatherRadarElapsed(lastStart + 1000));
            QCOMPARE(map.m_weatherRadar->m_weatherRadarFrameIndex, 0);
        }
        map.stopWeatherRadarAnimation();
        QGV::setNetworkManager(nullptr);
    }

    void globeRadarAboveDetailGeometry()
    {
        if (!qEnvironmentVariableIsSet("AETHERSDR_TEST_RADAR_GL")) {
            QSKIP("Opt in with AETHERSDR_TEST_RADAR_GL=1 and a native GUI platform");
        }
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        GlobeMapView globe;
        globe.resize(600, 400);
        globe.show();
        QTRY_VERIFY(globe.isValid());
        globe.cancelTileRequests();
        globe.m_navigation.reset(31, -79);
        globe.m_cameraDistance = 1.2F;
        globe.m_detailSelectionDirty = false;
        globe.m_atlas.fill(Qt::black);
        globe.m_atlasDirty = true;
        globe.m_terminatorVisible = false;
        globe.m_weatherRadarVisible = true;
        globe.makeCurrent();
        globe.m_visibleDetailKeys.clear();
        globe.m_detailTiles.clear();
        // The actual production detail mesh is finer (and radially higher)
        // than the global radar mesh. Opaque basemap triangles must not punch
        // holes in the overlay, regardless of which detail level is loaded.
        for (int y = 50; y <= 54; ++y) {
            for (int x = 34; x <= 37; ++x) {
                auto tile = std::make_shared<GlobeMapView::DetailTile>();
                tile->zoom = 7;
                tile->x = x;
                tile->y = y;
                tile->image = QImage(256, 256, QImage::Format_RGBA8888);
                tile->image.fill(Qt::black);
                globe.uploadDetailTile(*tile);
                const QString key = globe.detailTileKey(7, x, y);
                globe.m_detailTiles.insert(key, tile);
                globe.m_visibleDetailKeys.append(key);
            }
        }
        const QRectF bounds(-kRadarMercatorExtent, -kRadarMercatorExtent,
                            kRadarWorldWidth, kRadarWorldWidth);
        const QDateTime time = QDateTime::currentDateTimeUtc();
        QImage rain(512, 512, WeatherRadarTexture::kImageFormat);
        rain.fill(Qt::green);
        QTRY_VERIFY(globe.showWeatherRadarPlaybackFrame(rain, time, bounds));
        globe.acknowledgeWeatherRadarPlaybackFrame(1);
        const auto greenSamples = [&globe] {
            const QImage screen = globe.grabFramebuffer();
            int samples = 0;
            for (int y = screen.height() / 4; y < screen.height() * 3 / 4; y += 4) {
                for (int x = screen.width() / 4; x < screen.width() * 3 / 4; x += 4) {
                    samples += screen.pixelColor(x, y).green() > 150 ? 1 : 0;
                }
            }
            return samples;
        };
        const QSize screenSize = globe.grabFramebuffer().size();
        const int expected = ((screenSize.width() / 2 + 3) / 4)
            * ((screenSize.height() / 2 + 3) / 4);
        QCOMPARE(greenSamples(), expected);
        // Hold the final frame while the first frame of the next loop uploads
        // into inactive storage, including a genuinely clear next original.
        // Test final framebuffer pixels, not just the front texture's storage.
        QImage next(512, 1800, WeatherRadarTexture::kImageFormat);
        next.fill(Qt::transparent);
        globe.preloadWeatherRadarPlaybackFrame(next, time.addSecs(-300), bounds);
        for (int tick = 0; tick < 70; ++tick) {
            QCOMPARE(greenSamples(), expected);
        }
        QVERIFY(!globe.m_preloadedWeatherRadarAtlasDirty);
        // Disabling terrain depth alone would paint the far hemisphere over
        // clear near-side weather. Verify that hidden-side rain stays hidden.
        rain.fill(Qt::transparent);
        for (int y = 0; y < rain.height(); ++y) {
            for (int x = rain.width() / 2; x < rain.width(); ++x) {
                rain.setPixelColor(x, y, Qt::green);
            }
        }
        QTRY_VERIFY(globe.showWeatherRadarPlaybackFrame(rain, time.addSecs(300), bounds));
        globe.acknowledgeWeatherRadarPlaybackFrame(2);
        QCOMPARE(greenSamples(), 0);
        QGV::setNetworkManager(nullptr);
    }

    void failedLiveAtlasRetainsPixels()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        GlobeMapView globe;
        globe.cancelTileRequests();
        QImage retained = globe.m_weatherRadarAtlas;
        retained.fill(Qt::green);
        globe.m_weatherRadarAtlas = retained;
        globe.m_weatherRadarTextureBounds = QVector4D(.1F, .2F, .5F, .8F);
        globe.m_loadedWeatherRadarFrameId = QStringLiteral("retained-good-observation");
        globe.setWeatherRadarVisible(true);
        QVERIFY(globe.m_pendingWeatherRadarTileCount > 0);
        for (int i = 0; i < network.allReplies.size(); ++i) {
            if (network.allReplies.at(i) && !network.allReplies.at(i)->isFinished()) {
                network.allReplies.at(i)->complete(true);
            }
        }
        QCOMPARE(globe.m_pendingWeatherRadarTileCount, 0);
        QCOMPARE(globe.m_weatherRadarAtlas, retained);
        QCOMPARE(globe.m_loadedWeatherRadarFrameId, QStringLiteral("retained-good-observation"));
        QVERIFY(globe.weatherRadarLoadFailed());
        QGV::setNetworkManager(nullptr);
    }

    void globePlaybackResidency()
    {
        if (!qEnvironmentVariableIsSet("AETHERSDR_TEST_RADAR_GL")) {
            QSKIP("Opt in with AETHERSDR_TEST_RADAR_GL=1 and a native GUI platform");
        }
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        GlobeMapView globe;
        globe.resize(600, 400);
        globe.setWeatherRadarVisible(true);
        globe.show();
        QTRY_VERIFY(globe.isValid());
        const QRectF bounds(-kRadarMercatorExtent, -kRadarMercatorExtent,
                            kRadarWorldWidth, kRadarWorldWidth);
        const QDateTime start = QDateTime::currentDateTimeUtc().addSecs(-1800);
        const auto pixels = [&globe] {
            globe.makeCurrent();
            QOpenGLFunctions* gl = globe.context()->functions();
            QOpenGLFramebufferObject fbo(globe.m_radarTexture->width(),
                                         globe.m_radarTexture->height());
            fbo.bind();
            gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                GL_TEXTURE_2D, globe.m_radarTexture->textureId(), 0);
            QImage readback(fbo.size(), WeatherRadarTexture::kImageFormat);
            gl->glReadPixels(0, 0, readback.width(), readback.height(),
                GL_RGBA, GL_UNSIGNED_BYTE, readback.bits());
            fbo.release();
            return readback;
        };
        QImage original(512, 900, WeatherRadarTexture::kImageFormat);
        original.fill(Qt::green);
        QTRY_VERIFY(globe.showWeatherRadarPlaybackFrame(original, start, bounds));
        globe.acknowledgeWeatherRadarPlaybackFrame(1);
        QCOMPARE(pixels(), original);
        // Same timestamp, extent and dimensions do NOT prove texture identity.
        // A replacement may contain different pixels (including no-rain data).
        QImage replacement = original;
        replacement.fill(Qt::red);
        QTRY_VERIFY(globe.showWeatherRadarPlaybackFrame(replacement, start, bounds));
        QCOMPARE(pixels(), replacement);
        for (int i = 1; i <= 18; ++i) {
            QImage next(512 + (i % 3) * 32, 900, WeatherRadarTexture::kImageFormat);
            next.fill(QColor(10 + i, 130 + i, 40 + i, 255));
            const QDateTime time = start.addSecs((i % 6) * 300);
            globe.preloadWeatherRadarPlaybackFrame(next, time, bounds);
            // Incomplete stripes cannot replace or erase the displayed image.
            QCOMPARE(pixels(), replacement);
            QTRY_VERIFY(globe.showWeatherRadarPlaybackFrame(next, time, bounds));
            globe.acknowledgeWeatherRadarPlaybackFrame(i + 1);
            QCOMPARE(pixels(), next);
            replacement = next;
        }
        globe.setWeatherRadarSource(WeatherRadarSource::currentNoaaFrame());
        QVERIFY(globe.m_weatherRadarReplies.isEmpty()); // No live atlas can overwrite playback.
        QCOMPARE(pixels(), replacement);
        QImage clear = replacement;
        clear.fill(Qt::transparent);
        QTRY_VERIFY(globe.showWeatherRadarPlaybackFrame(clear, start.addSecs(2100), bounds));
        QCOMPARE(pixels(), clear); // Genuine no-rain observations are not suppressed.
        QGV::setNetworkManager(nullptr);
    }

    void projectionAndRollingHistory()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        MapDisplayWidget map;
        prepare(map, network);
        finishAll(map, network);
        QTRY_COMPARE(map.m_weatherRadar->weatherRadarPlaybackFrameCount(), 6);
        map.m_weatherRadar->m_weatherRadarPlaybackTimer->stop();
        map.m_weatherRadar->m_weatherRadarTimer->stop();
        map.setWeatherRadarPlaybackSpeed(137);
        const auto frames = map.m_weatherRadar->frameValues(&WeatherRadarFrame::time);
        const auto bytes = map.m_weatherRadar->frameValues(&WeatherRadarFrame::bytes);
        const auto bounds = map.m_weatherRadar->frameValues(&WeatherRadarFrame::bounds);
        const auto elapsed = map.m_weatherRadar->m_weatherRadarPlaybackCadence.requestedElapsedMs();
        QSignalSpy state(&map, &MapDisplayWidget::weatherRadarAnimationStateChanged);
        // Hidden widget: this state-machine test never requires a GL context.
        map.hide();
        map.setProjectionMode(MapDisplayWidget::ProjectionMode::Globe);
        QCOMPARE(map.projectionMode(), MapDisplayWidget::ProjectionMode::Globe);
        QVERIFY(map.m_weatherRadar->m_weatherRadarPlaybackRequested && map.m_weatherRadar->m_weatherRadarAnimating);
        QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::time), frames);
        QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::bytes), bytes);
        QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::bounds), bounds);
        QCOMPARE(map.m_weatherRadar->m_weatherRadarPlaybackCadence.requestedElapsedMs(), elapsed);
        QCOMPARE(map.m_weatherRadar->weatherRadarRendererBounds(bounds.first()), bounds.first());
        map.setProjectionMode(MapDisplayWidget::ProjectionMode::Flat);
        QVERIFY(state.isEmpty());
        QCOMPARE(map.m_weatherRadar->m_weatherRadarPlaybackSpeedPercent, 137);
        QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::bounds), bounds);
        QCOMPARE(map.m_weatherRadar->weatherRadarRendererBounds(bounds.first()),
            WeatherRadarSource::conventionalBoundsFromQgv(bounds.first()));
        map.m_weatherRadar->m_weatherRadarRebufferTimer->stop();

        map.m_weatherRadar->m_weatherRadarHistoryHours = 1;
        const QDateTime newest = frames.last().addSecs(3600);
        map.m_weatherRadar->appendWeatherRadarObservations({
            {frames.last(), map.m_weatherRadar->m_frames.last().sampleTime,
                map.m_weatherRadar->m_frames.last().rasterIds},
            {newest, newest.addSecs(150), {1000}}});
        QCOMPARE(map.m_weatherRadar->weatherRadarPlaybackFrameCount(), 6);
        QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::bytes).mid(0, 6), bytes);
        finishAll(map, network);
        QVERIFY(map.m_weatherRadar->m_weatherRadarBufferFinalizationPending);
        // Pruning the reusable cache must not destroy the active frame bytes.
        map.m_weatherRadar->m_weatherRadarFrameCache.clear();
        map.m_weatherRadar->applyFinalizedWeatherRadarBuffering();
        QCOMPARE(map.m_weatherRadar->m_frames.size(), 2);
        QCOMPARE(map.m_weatherRadar->m_frames.first().time, frames.last());
        QCOMPARE(map.m_weatherRadar->m_frames.first().bounds, bounds.last());
        QCOMPARE(map.m_weatherRadar->m_frames.first().bytes, bytes.last());
        QVERIFY(!map.m_weatherRadar->m_frames.last().bytes.isEmpty());
        QCOMPARE(map.m_weatherRadar->weatherRadarPlaybackFrameCount(), 2);
        QVERIFY(map.m_weatherRadar->m_weatherRadarAnimating);
        map.stopWeatherRadarAnimation();
        map.setProjectionMode(MapDisplayWidget::ProjectionMode::Globe);
        QVERIFY(!map.m_weatherRadar->m_weatherRadarPlaybackRequested);
        map.setProjectionMode(MapDisplayWidget::ProjectionMode::Flat);
        QVERIFY(!map.m_weatherRadar->m_weatherRadarPlaybackRequested);
        QGV::setNetworkManager(nullptr);
    }

    void geometryAndPriority()
    {
        const WeatherRadarViewGeometry view{QRectF(-11000000, 4000000, 600000, 400000), QSize(600, 400)};
        const WeatherRadarViewGeometry padded = weatherRadarPaddedView(view, 2048);
        QVERIFY(padded.covers(view));
        QVERIFY(padded.covers({view.bounds.translated(10000, 10000), view.size}));
        QVERIFY(!padded.covers({view.bounds.translated(500000, 0), view.size}));
        QVERIFY(!padded.covers({view.bounds, QSize(2400, 1600)}));
        const WeatherRadarViewGeometry wide{QRectF(-kRadarMercatorExtent, -1e7, kRadarWorldWidth, 2e7), QSize(2048, 1024)};
        const WeatherRadarViewGeometry capped = weatherRadarPaddedView(wide, 2048);
        QVERIFY(capped.size.width() <= 2048 && capped.size.height() <= 2048);
        QCOMPARE(capped.bounds.left(), -kRadarMercatorExtent);
        QCOMPARE(capped.bounds.right(), kRadarMercatorExtent);
        QVERIFY(capped.covers(wide)); // Padding cannot quietly lower resolution.
        QVector<int> queue{0, 1, 2, 3, 4, 5};
        QCOMPARE(weatherRadarTakePriorityFrame(queue, 4, 6), 4);
        QCOMPARE(weatherRadarTakePriorityFrame(queue, 4, 6), 5);
        QCOMPARE(weatherRadarTakePriorityFrame(queue, 4, 6), 0);
        QCOMPARE(weatherRadarTakePriorityFrame(queue, 1, 6), 1);
    }

    void queuedExportRetainsItsGeometryAfterCacheEviction()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        MapDisplayWidget map;
        prepare(map, network);
        // A covering cached view selected earlier can disappear from the RAM
        // cache while waiting behind four requests. Its re-fetch URL still
        // refers to that older extent/size, not the batch's current geometry.
        const WeatherRadarViewGeometry selected{QRectF(-11e6, 4e6, 3e6, 2e6), QSize(128, 96)};
        const int index = 4;
        const auto source = WeatherRadarSource::historicalNoaaFrame(
            map.m_weatherRadar->m_frames.at(index).time, map.m_weatherRadar->m_frames.at(index).sampleTime,
            map.m_weatherRadar->m_frames.at(index).rasterIds);
        const QUrl selectedUrl = source.imageUrl(selected.bounds, selected.size);
        map.m_weatherRadar->m_frames[index].url = selectedUrl;
        map.m_weatherRadar->m_frames[index].requestGeometry = selected;
        network.pending()->complete();
        QTRY_VERIFY(network.pending(selectedUrl));
        network.pending(selectedUrl)->complete();
        QTRY_VERIFY(!map.m_weatherRadar->m_frames.at(index).bytes.isEmpty());
        QCOMPARE(map.m_weatherRadar->m_weatherRadarDecodedImages.value(index).size(), selected.size);
        QCOMPARE(map.m_weatherRadar->m_frames.at(index).bounds, selected.bounds);
        QGV::setNetworkManager(nullptr);
    }

    void retainedTransparentTiles()
    {
        QGVMap map;
        map.resize(800, 600);
        map.show();
        QTest::qWait(10);
        auto* layer = new InjectedTileLayer();
        layer->setTransparentFallbackEnabled(true);
        layer->setVisibleZoomLayersBelowCurrent(3);
        layer->setVisibleZoomLayersAboveCurrent(3);
        map.addItem(layer);
        const QGV::GeoTilePos parent(2, QPoint(1, 1));
        const QRectF region = map.getProjection()->geoToProj(parent.toGeoRect());
        map.cameraTo(QGVCameraActions(&map).scaleTo(500.0 / region.height())
            .moveTo(region.center()), false);
        layer->process();
        layer->deliver(parent);
        QVERIFY(layer->tileUncoveredPath(parent).contains(region.center()));
        layer->zoom = 4;
        layer->process();
        // Eight of sixteen grandchildren cannot replace the whole parent.
        for (int y = 4; y < 6; ++y) {
            for (int x = 4; x < 8; ++x) {
                layer->deliver(QGV::GeoTilePos(4, QPoint(x, y)));
            }
        }
        const QPointF missing = map.getProjection()->geoToProj(
            QGV::GeoTilePos(4, QPoint(7, 7)).toGeoRect()).center();
        const QPointF delivered = map.getProjection()->geoToProj(
            QGV::GeoTilePos(4, QPoint(4, 4)).toGeoRect()).center();
        QVERIFY(layer->tileUncoveredPath(parent).contains(missing));
        QVERIFY(!layer->tileUncoveredPath(parent).contains(delivered));
        // Zooming out before a new coarse tile arrives retains the detail.
        layer->zoom = 1;
        layer->process();
        const QGV::GeoTilePos child(4, QPoint(4, 4));
        QVERIFY(layer->tileUncoveredPath(child).contains(delivered));
        layer->deliver(QGV::GeoTilePos(1, QPoint(0, 0)));
        QVERIFY(layer->tileUncoveredPath(child).isEmpty());
    }

    void overlayLoadingMessagesCoexist()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        MapDisplayWidget map;
        map.resize(800, 500);
        map.show();
        const QString accessibleName = map.m_weatherRadarLoadingLabel->accessibleName();
        map.m_cityLightsVisible = true;
        emit map.cityLightsStatusChanged(QStringLiteral("Loading city lights…"));
        QVERIFY(!map.m_weatherRadarLoadingLabel->isVisible());
        QTRY_VERIFY(map.m_weatherRadarLoadingLabel->isVisible());
        QCOMPARE(map.m_weatherRadarLoadingLabel->text(), QStringLiteral("Loading city lights…"));
        map.m_weatherRadar->m_weatherRadarVisible = true;
        map.m_weatherRadarLoadingText = QStringLiteral("Loading radar… 2/6");
        map.m_weatherRadarLoadingAnnouncement = QStringLiteral("Loading radar…");
        map.updateOverlayLoadingStatus();
        QCOMPARE(map.m_weatherRadarLoadingLabel->text(),
                 QStringLiteral("Loading radar… 2/6\nLoading city lights…"));
        QCOMPARE(map.m_weatherRadarLoadingLabel->accessibleName(), accessibleName);
        QCOMPARE(map.m_weatherRadarLoadingLabel->accessibleDescription(),
                 QStringLiteral("Loading radar…\nLoading city lights…"));
        QVERIFY(map.m_weatherRadarLoadingLabel->testAttribute(Qt::WA_TransparentForMouseEvents));
        map.m_weatherRadar->m_weatherRadarVisible = false;
        map.m_weatherRadar->updateWeatherRadarLoadingStatus();
        QCOMPARE(map.m_weatherRadarLoadingLabel->text(), QStringLiteral("Loading city lights…"));
        emit map.cityLightsStatusChanged(QString());
        QVERIFY(!map.m_weatherRadarLoadingLabel->isVisible());
        QCOMPARE(map.m_weatherRadarLoadingLabel->accessibleName(), accessibleName);
        QVERIFY(map.m_weatherRadarLoadingLabel->accessibleDescription().isEmpty());
        emit map.cityLightsStatusChanged(QStringLiteral("Loading city lights…"));
        emit map.cityLightsStatusChanged(QString());
        QTest::qWait(400);
        QVERIFY(!map.m_weatherRadarLoadingLabel->isVisible());
        QGV::setNetworkManager(nullptr);
    }

    void progressiveZoomAndFailures()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        MapDisplayWidget map;
        prepare(map, network);
        QSignalSpy frames(&map, &MapDisplayWidget::weatherRadarFrameChanged);
        QCOMPARE(network.requests.size(), 4); // Bounded even on a stalled link.
        map.m_weatherRadar->updateWeatherRadarLoadingStatus();
        QVERIFY(!map.m_weatherRadarLoadingLabel->isVisible());
        QTest::qWait(340);
        map.m_weatherRadar->updateWeatherRadarLoadingStatus();
        QVERIFY(map.m_weatherRadarLoadingLabel->isVisible());
        QCOMPARE(map.m_weatherRadarLoadingLabel->text(), QStringLiteral("Loading radar… 0/6"));
        QVERIFY(map.m_weatherRadarLoadingLabel->testAttribute(Qt::WA_TransparentForMouseEvents));
        QCOMPARE(map.m_weatherRadarLoadingLabel->focusPolicy(), Qt::NoFocus);
        QVERIFY(std::abs(map.m_weatherRadarLoadingLabel->geometry().center().x() - map.width()/2) < 2);

        QVERIFY(network.pending(map.m_weatherRadar->m_frames.at(0).url));
        network.pending(map.m_weatherRadar->m_frames.at(0).url)->complete();
        QTRY_VERIFY(map.m_weatherRadar->m_weatherRadarDecodedImages.contains(0));
        QVERIFY(!frames.isEmpty()); // A single complete image is displayed now.
        QVERIFY(!map.m_weatherRadar->m_weatherRadarAnimating);
        network.pending(map.m_weatherRadar->m_frames.at(1).url)->complete();
        QTRY_VERIFY(map.m_weatherRadar->m_weatherRadarAnimating); // No five-frame barrier.
        QVERIFY(map.m_weatherRadar->m_weatherRadarPlaybackTimer->isActive());
        QTRY_VERIFY(map.m_weatherRadar->m_weatherRadarFrameIndex == 1);
        finishAll(map, network);
        QVERIFY(map.m_weatherRadar->m_weatherRadarNetworkRequestsComplete);
        QTRY_COMPARE(map.m_weatherRadar->weatherRadarPlaybackFrameCount(), 6);

        QGVMap* flat = map.m_flatView->findChild<QGVMap*>();
        const double originalScale = flat->getCamera().scale();
        const QPointF originalCenter = flat->getCamera().projRect().center();
        const QVector<QRectF> oldBounds = map.m_weatherRadar->frameValues(&WeatherRadarFrame::bounds);
        const QVector<QString> oldKeys = map.m_weatherRadar->frameValues(&WeatherRadarFrame::cacheKey);
        const qint64 oldClock = map.m_weatherRadar->m_weatherRadarPlaybackClock.elapsed();
        flat->cameraTo(QGVCameraActions(flat).scaleTo(originalScale * 2), false);
        QCoreApplication::processEvents();
        const int beforeRequests = network.requests.size();
        const int displayed = map.m_weatherRadar->m_weatherRadarFrameIndex;
        map.m_weatherRadar->rebufferWeatherRadarPlayback();
        map.m_weatherRadar->m_weatherRadarRebufferTimer->stop();
        QVERIFY(map.m_weatherRadar->m_weatherRadarAnimating);
        QVERIFY(map.m_weatherRadar->m_weatherRadarPlaybackRequested);
        QVERIFY(map.m_weatherRadar->m_weatherRadarPlaybackTimer->isActive());
        QVERIFY(map.m_weatherRadar->m_weatherRadarPlaybackClock.elapsed() >= oldClock);
        QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::bounds), oldBounds);
        QCOMPARE(map.m_weatherRadar->frameValues(&WeatherRadarFrame::cacheKey), oldKeys);
        QCOMPARE(network.requests.size(), beforeRequests + 4);
        QCOMPARE(network.requests.at(beforeRequests), map.m_weatherRadar->m_frames.at(displayed).url);
        QSignalSpy playing(&map, &MapDisplayWidget::weatherRadarAnimationStateChanged);
        QSignalSpy loading(&map, &MapDisplayWidget::weatherRadarTimelineLoadingChanged);
        network.pending(map.m_weatherRadar->m_frames.at(displayed).url)->complete();
        QTRY_VERIFY(map.m_weatherRadar->m_frames.at(displayed).cacheKey != oldKeys.at(displayed));
        const int neighbor = (displayed + 1) % 6;
        QVERIFY(map.m_weatherRadar->m_frames.at(displayed).bounds != oldBounds.at(displayed));
        QCOMPARE(map.m_weatherRadar->m_frames.at(neighbor).bounds, oldBounds.at(neighbor));
        QVERIFY(!map.m_weatherRadar->m_weatherRadarDecodedImages.value(displayed).isNull());
        QVERIFY(!map.m_weatherRadar->m_frames.at(neighbor).bytes.isEmpty());
        QVERIFY(playing.isEmpty() && loading.isEmpty()); // Zoom cannot change Play/Pause.
        const qint64 elapsed = map.m_weatherRadar->m_weatherRadarPlaybackCadence.presentedElapsedMs();
        QTest::qWait(200);
        QVERIFY(map.m_weatherRadar->m_weatherRadarPlaybackCadence.presentedElapsedMs() > elapsed);

        QVERIFY(network.pending());
        network.pending()->complete(true);
        finishAll(map, network);
        QVERIFY(!map.m_weatherRadar->m_weatherRadarDownloadFailed.isEmpty());
        for (const QByteArray& bytes : map.m_weatherRadar->frameValues(&WeatherRadarFrame::bytes)) {
            QVERIFY(!bytes.isEmpty()); // Failure is not "no rain" and not a blank.
        }
        map.m_weatherRadar->updateWeatherRadarLoadingStatus();
        QTest::qWait(340);
        map.m_weatherRadar->updateWeatherRadarLoadingStatus();
        QCOMPARE(map.m_weatherRadarLoadingLabel->text(), QStringLiteral("Loading radar data failed"));
        map.m_weatherRadar->m_weatherRadarRebufferTimer->stop();

        // A late old-generation result must not re-georeference current data.
        const int generation = map.m_weatherRadar->m_weatherRadarBufferGeneration;
        const auto frame = map.m_weatherRadar->m_weatherRadarFrameCache.value(map.m_weatherRadar->m_frames.at(0).cacheKey);
        const QRectF retained = map.m_weatherRadar->m_frames.at(0).bounds;
        ++map.m_weatherRadar->m_weatherRadarBufferGeneration;
        map.m_weatherRadar->acceptWeatherRadarDownload(generation, 0, oldKeys.at(0), frame);
        QCOMPARE(map.m_weatherRadar->m_frames.at(0).bounds, retained);

        // Returning to the previous view reuses all six original exports.
        flat->cameraTo(QGVCameraActions(flat).scaleTo(originalScale).moveTo(originalCenter), false);
        QCoreApplication::processEvents();
        const int beforeReturn = network.requests.size();
        map.m_weatherRadar->rebufferWeatherRadarPlayback();
        QTRY_VERIFY(map.m_weatherRadar->m_weatherRadarNetworkRequestsComplete);
        QCOMPARE(network.requests.size(), beforeReturn);
        QVERIFY(map.m_weatherRadar->m_weatherRadarDownloadFailed.isEmpty());
        map.m_weatherRadar->m_weatherRadarRebufferTimer->stop();
        flat->cameraTo(QGVCameraActions(flat).moveTo(originalCenter + QPointF(1000, 1000)), false);
        QCoreApplication::processEvents();
        map.m_weatherRadar->rebufferWeatherRadarPlayback();
        QCOMPARE(network.requests.size(), beforeReturn);
        map.stopWeatherRadarAnimation();
        map.m_weatherRadar->m_weatherRadarVisible = false;
        map.m_weatherRadar->updateWeatherRadarLoadingStatus();
        QVERIFY(!map.m_weatherRadarLoadingLabel->isVisible());
        QGV::setNetworkManager(nullptr);
    }
};

} // namespace AetherSDR

QTEST_MAIN(AetherSDR::WeatherRadarLoadingTest)
#include "weather_radar_loading_test.moc"
