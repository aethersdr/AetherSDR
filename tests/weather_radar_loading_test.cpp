#include "gui/map/MapDisplayWidget.h"
#include "gui/map/GlobeMapView.h"
#include "gui/map/WeatherRadarTexture.h"
#include "gui/map/WeatherRadarTileLayer.h"

#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVMapQGView.h>
#include <QGeoView/QGVProjection.h>
#include <QGeoView/QGVLayerTiles.h>
#include <QGeoView/Raster/QGVImage.h>
#include <QApplication>
#include <QBuffer>
#include <QLabel>
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
        map.m_weatherRadarNetwork = &network;
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
        map.m_weatherRadarVisible = true;
        map.m_weatherRadarPlaybackRequested = true;
        const QDateTime start = QDateTime::currentDateTimeUtc().addSecs(-count * 300);
        for (int i = 0; i < count; ++i) {
            map.m_weatherRadarFrames.append(start.addSecs(i * 300));
            map.m_weatherRadarFrameSampleTimes.append(start.addSecs(i * 300 + 150));
            map.m_weatherRadarFrameRasterIds.append(QVector<qint64>{100 + i});
        }
        map.bufferWeatherRadarFrames();
    }
    static void finishAll(MapDisplayWidget& map, ControlledRadarNetwork& network)
    {
        QElapsedTimer deadline;
        deadline.start();
        while (!map.m_weatherRadarNetworkRequestsComplete && deadline.elapsed() < 5000) {
            if (auto* reply = network.pending()) {
                reply->complete();
            }
            QTest::qWait(10);
        }
    }

private slots:
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
        const QVector<QDateTime> originalFrames = map.m_weatherRadarFrames;
        QVector<WeatherRadarObservation> catalog;
        for (int i = 0; i < originalFrames.size(); ++i) {
            catalog.append({originalFrames.at(i), map.m_weatherRadarFrameSampleTimes.at(i),
                            map.m_weatherRadarFrameRasterIds.at(i)});
        }
        network.pending(map.m_weatherRadarFrameUrls.at(failedIndex))->complete(true);
        finishAll(map, network);
        map.m_weatherRadarPlaybackTimer->stop();
        map.applyFinalizedWeatherRadarBuffering();
        QCOMPARE(map.m_weatherRadarFrames.size(), 5);
        QVERIFY(map.m_weatherRadarRebufferTimer->isActive());
        const QVector<QDateTime> activeFrames = map.m_weatherRadarFrames;
        const QVector<int> activeDurations = map.m_weatherRadarActiveSegmentDurationsMs;
        const int requests = network.requests.size();
        map.rebufferWeatherRadarPlayback();
        QVERIFY(map.m_weatherRadarTimelineReply);
        map.cancelWeatherRadarTimelineRequest();
        map.appendWeatherRadarObservations(catalog);
        QCOMPARE(network.requests.size(), requests + 1); // Only the missing image.
        QCOMPARE(map.m_weatherRadarFrames.mid(0, 5), activeFrames);
        QCOMPARE(map.m_weatherRadarActiveSegmentDurationsMs, activeDurations);
        QCOMPARE(map.weatherRadarPlaybackFrameCount(), 5);
        // A view refresh while the older retry is staged must not reject its
        // temporarily unsorted tail or change the active movie's cadence.
        map.cancelWeatherRadarFrameRequests();
        map.bufferWeatherRadarFrames(true);
        QVERIFY(map.m_weatherRadarAnimating);
        QCOMPARE(map.m_weatherRadarActiveSegmentDurationsMs, activeDurations);
        finishAll(map, network);
        QVERIFY(map.m_weatherRadarBufferFinalizationPending);
        QCOMPARE(map.weatherRadarPlaybackFrameCount(), 5);
        map.applyFinalizedWeatherRadarBuffering(); // The loop-restart operation.
        QCOMPARE(map.m_weatherRadarFrames, originalFrames);
        QCOMPARE(map.weatherRadarPlaybackFrameCount(), 6);
        QVERIFY(map.m_weatherRadarRetryFrames.isEmpty());
        QVERIFY(map.m_weatherRadarDownloadFailed.isEmpty());
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
        QTRY_COMPARE_WITH_TIMEOUT(network.allReplies.size(), requests + 1, 3000);
        QVERIFY(ready.isEmpty());
        network.allReplies.last()->complete();
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(!layer->loadFailed());
        QCOMPARE(layer->pendingRequestCount(), 0);
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
        map.applyFinalizedWeatherRadarBuffering();
        QCOMPARE(map.weatherRadarPlaybackFrameCount(), 6);
        map.m_weatherRadarPlaybackTimer->stop();
        const auto frames = map.m_weatherRadarFrames;
        const auto bytes = map.m_weatherRadarBufferedBytes;
        const int downloads = network.requests.size();
        QVector<WeatherRadarObservation> catalog;
        for (int i = 1; i < frames.size(); ++i) {
            catalog.append({frames.at(i), map.m_weatherRadarFrameSampleTimes.at(i),
                            map.m_weatherRadarFrameRasterIds.at(i)});
        }
        map.appendWeatherRadarObservations(catalog);
        QCOMPARE(map.m_weatherRadarFrames, frames); // Do not renumber mid-loop.
        QVERIFY(map.m_weatherRadarBufferFinalizationPending);
        map.applyFinalizedWeatherRadarBuffering();
        QCOMPARE(map.m_weatherRadarFrames, frames.mid(1));
        QCOMPARE(map.m_weatherRadarBufferedBytes, bytes.mid(1));
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
        map.applyFinalizedWeatherRadarBuffering();
        QCOMPARE(map.weatherRadarPlaybackFrameCount(), 6);
        map.m_weatherRadarPlaybackTimer->stop();
        if (globe) {
            map.hide();
            map.setProjectionMode(MapDisplayWidget::ProjectionMode::Globe);
        }
        map.m_weatherRadarRebufferTimer->stop();
        const auto frames = map.m_weatherRadarFrames;
        const auto bytes = map.m_weatherRadarBufferedBytes;
        // Reproduce NOAA expiring an immutable raster BETWEEN catalog fetch
        // and a zoom export: HTTP 200, correctly sized PNG, every alpha zero.
        map.m_weatherRadarFrameCache.clear();
        map.cancelWeatherRadarFrameRequests();
        map.m_weatherRadarDetailRefresh = true;
        map.m_weatherRadarRequestedView = {map.weatherRadarCurrentView().bounds, QSize(256, 256)};
        map.m_weatherRadarFrameCacheKeys[0].clear();
        map.m_weatherRadarFrameUrls[0] = WeatherRadarSource::historicalNoaaFrame(
            frames.first(), map.m_weatherRadarFrameSampleTimes.first(), {100}).imageUrl(
                map.m_weatherRadarRequestedView.bounds, map.m_weatherRadarRequestedView.size);
        // Feed the same production decode path as the network reply.
        QImage clear(map.m_weatherRadarRequestedView.size, WeatherRadarTexture::kImageFormat);
        clear.fill(Qt::transparent);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        clear.save(&buffer, "PNG");
        const QString key = map.m_weatherRadarFrameUrls.first().toString(QUrl::FullyEncoded);
        map.m_weatherRadarNetworkRequestsComplete = false;
        map.decodeWeatherRadarDownload(0, png, key, map.m_weatherRadarRequestedView);
        QTRY_VERIFY(network.pendingValidation() != nullptr);
        QCOMPARE(map.m_weatherRadarBufferedBytes.first(), bytes.first());
        if (result == 3) {
            const QPointer<ControlledRadarReply> verification = network.pendingValidation();
            map.stopWeatherRadarAnimation();
            // A late reply from a stopped/changed generation cannot resurrect
            // playback or install its unverified empty pixels into the cache.
            if (verification) {
                verification->completeJson(R"({"objectIds":[100]})");
            }
            QCoreApplication::processEvents();
            QVERIFY(!map.m_weatherRadarPlaybackRequested);
            QVERIFY(map.m_weatherRadarBufferedBytes.isEmpty());
            QVERIFY(!map.m_weatherRadarFrameCache.contains(key));
            QGV::setNetworkManager(nullptr);
            return;
        }
        if (result == 2) {
            network.pendingValidation()->complete(true);
        } else {
            network.pendingValidation()->completeJson(result == 1
                ? R"({"objectIds":[100]})" : R"({"objectIds":[]})");
        }
        QTRY_VERIFY(!map.m_weatherRadarDownloadDecodePending.contains(0));
        map.updateWeatherRadarLoadingStatus();
        if (result == 0) {
            // Confirmed source expiration is normal rolling-history upkeep,
            // not a download failure and not a reason to retry that export.
            QVERIFY(!map.m_weatherRadarDownloadFailed.contains(0));
            QVERIFY(!map.m_weatherRadarRebufferTimer->isActive());
            QVERIFY(map.m_weatherRadarLoadingLabel->text() != QStringLiteral("Loading radar data failed"));
        } else if (result == 2) {
            QCOMPARE(map.m_weatherRadarLoadingLabel->text(), QStringLiteral("Loading radar data failed"));
            QVERIFY(map.m_weatherRadarRebufferTimer->isActive());
        }
        if (result == 1) {
            QCOMPARE(map.m_weatherRadarBufferedBytes.first(), png);
            QVERIFY(map.m_weatherRadarFrameCache.contains(key));
            // Trusted clear originals may be replayed from compressed cache
            // without another network verification on every loop.
            map.m_weatherRadarFrameCache[key].decodedImage = {};
            map.m_weatherRadarBufferQueue = {0};
            map.requestNextWeatherRadarBufferedFrames();
            QTRY_VERIFY(!map.m_weatherRadarDownloadDecodePending.contains(0));
            QVERIFY(network.pendingValidation() == nullptr);
        } else {
            QCOMPARE(map.m_weatherRadarBufferedBytes.first(), bytes.first());
            QVERIFY(!map.m_weatherRadarFrameCache.contains(key));
            map.m_weatherRadarRebufferTimer->stop();
            if (result == 0) {
                QVERIFY(map.m_weatherRadarBufferFinalizationPending);
                map.applyFinalizedWeatherRadarBuffering();
                QCOMPARE(map.m_weatherRadarFrames, frames.mid(1));
                QCOMPARE(map.m_weatherRadarBufferedBytes, bytes.mid(1));
            } else {
                QCOMPARE(map.m_weatherRadarFrames, frames);
                QVERIFY(map.m_weatherRadarDownloadFailed.contains(0));
            }
        }
        map.stopWeatherRadarAnimation();
        QGV::setNetworkManager(nullptr);
    }

    void flatControllerLoopRetainsEveryPaint()
    {
        if (!qEnvironmentVariableIsSet("AETHERSDR_TEST_RADAR_GL")) {
            QSKIP("Opt in with AETHERSDR_TEST_RADAR_GL=1 and a native GUI platform");
        }
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        MapDisplayWidget map;
        map.m_weatherRadarHistoryHours = 4;
        prepare(map, network, 18, true);
        QOpenGLWidget* viewport = map.m_flatView->findChild<QOpenGLWidget*>();
        QVERIFY(viewport != nullptr);
        QTRY_VERIFY(viewport->isValid());
        map.setWeatherRadarPlaybackSpeed(500);
        QElapsedTimer deadline;
        deadline.start();
        while (!map.m_weatherRadarNetworkRequestsComplete && deadline.elapsed() < 5000) {
            if (ControlledRadarReply* reply = network.pending()) {
                reply->complete(false, Qt::green);
            }
            QTest::qWait(10);
        }
        QTRY_COMPARE(map.weatherRadarPlaybackFrameCount(), 18);
        QTRY_VERIFY(map.m_weatherRadarPresentedImageKey != 0);
        map.m_weatherRadarFrameCache.clear();
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
                if (map.m_weatherRadarFrameIndex == 0 && previousIndex > 0) {
                    ++restarts;
                }
                previousIndex = map.m_weatherRadarFrameIndex;
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
        map.m_weatherRadarHistoryHours = 4;
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
        while (!map.m_weatherRadarNetworkRequestsComplete && deadline.elapsed() < 5000) {
            if (auto* reply = network.pending()) {
                reply->complete(false, Qt::green);
            }
            QTest::qWait(10);
        }
        QTRY_COMPARE(map.weatherRadarPlaybackFrameCount(), 18);
        QTRY_VERIFY(map.m_weatherRadarPresentedImageKey != 0);
        // Exercise real PNG re-decodes/new QImage identities on every wrap,
        // not only six images held forever by the cache or the test itself.
        map.m_weatherRadarFrameCache.clear();
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
                qWarning() << "Blank controller paint" << map.m_weatherRadarFrameIndex
                    << globe.m_radarTextureFrameTime << int(rgba[0]) << int(rgba[1]) << int(rgba[2]);
            }
            if (map.m_weatherRadarFrameIndex == 0 && previousIndex > 0) {
                ++restarts;
            }
            previousIndex = map.m_weatherRadarFrameIndex;
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
        map.m_weatherRadarNetwork = &network;
        map.m_weatherRadarVisible = true;
        QSignalSpy errors(&map, &MapDisplayWidget::weatherRadarAnimationError);
        map.startWeatherRadarAnimation(1);
        QVERIFY(map.m_weatherRadarTimelineReply);
        static_cast<ControlledRadarReply*>(map.m_weatherRadarTimelineReply)->complete(true);
        QVERIFY(map.weatherRadarAnimating()); // Includes requested/retrying playback.
        QVERIFY(map.m_weatherRadarTimelineFailed);
        QVERIFY(map.m_weatherRadarRebufferTimer->isActive());
        QVERIFY(errors.isEmpty());
        map.m_weatherRadarRebufferTimer->stop();
        map.rebufferWeatherRadarPlayback(); // Same callback as the five-second retry.
        QVERIFY(map.m_weatherRadarTimelineReply);
        QCOMPARE(map.m_weatherRadarTimelineReply->url(), WeatherRadarSource::noaaTimelineUrl());
        map.stopWeatherRadarAnimation();
        QVERIFY(!map.m_weatherRadarRebufferTimer->isActive());
        QVERIFY(!map.m_weatherRadarTimelineReply);
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
        QTRY_COMPARE(map.weatherRadarPlaybackFrameCount(), 6);
        map.m_weatherRadarPlaybackTimer->stop();
        map.m_weatherRadarTimer->stop();
        const auto bytes = map.m_weatherRadarBufferedBytes;
        const auto keys = map.m_weatherRadarFrameCacheKeys;
        const auto bounds = map.m_weatherRadarFrameBounds;
        const int generation = map.m_weatherRadarBufferGeneration;
        const int requests = network.requests.size();
        for (const int speed : {25, 73, 400, 500, 100}) {
            map.setWeatherRadarPlaybackSpeed(speed);
            map.rebufferWeatherRadarPlayback(); // Even a queued view callback is a no-op.
            QCOMPARE(map.m_weatherRadarPlaybackSpeedPercent, speed);
            QCOMPARE(map.m_weatherRadarBufferGeneration, generation);
            QCOMPARE(map.m_weatherRadarBufferedBytes, bytes);
            QCOMPARE(map.m_weatherRadarFrameCacheKeys, keys);
            QCOMPARE(map.m_weatherRadarFrameBounds, bounds);
            QCOMPARE(network.requests.size(), requests);

            qint64 lastStart = 0;
            for (const int duration : map.m_weatherRadarActiveSegmentDurationsMs) {
                lastStart += duration;
            }
            // Enter the actual final original, then hold it through many
            // ticks and a speed change. Neither preloading frame zero nor a
            // changed clock is permission to clear the displayed image.
            map.ensureWeatherRadarDecodeAhead(5);
            QTRY_VERIFY(map.m_weatherRadarDecodedImages.contains(5));
            map.m_weatherRadarPlaybackCadence.reset(lastStart,
                map.m_weatherRadarPlaybackClock.elapsed());
            QVERIFY(map.tryPresentWeatherRadarElapsed(lastStart));
            // Seed an acknowledged final frame for the clock-only test. The
            // native framebuffer test separately checks actual retained pixels.
            map.m_weatherRadarPlaybackCadence.rebaseElapsed(lastStart);
            const qint64 imageKey = map.m_weatherRadarPresentedImageKey;
            for (int hold = 16; hold < 1000; hold += 16) {
                QVERIFY(map.tryPresentWeatherRadarElapsed(lastStart + hold));
                QCOMPARE(map.m_weatherRadarFrameIndex, 5);
                QCOMPARE(map.m_weatherRadarPresentedImageKey, imageKey);
            }
            QVERIFY(map.tryPresentWeatherRadarElapsed(lastStart + 1000));
            QCOMPARE(map.m_weatherRadarFrameIndex, 0);
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
        QTRY_COMPARE(map.weatherRadarPlaybackFrameCount(), 6);
        map.m_weatherRadarPlaybackTimer->stop();
        map.m_weatherRadarTimer->stop();
        map.setWeatherRadarPlaybackSpeed(137);
        const auto frames = map.m_weatherRadarFrames;
        const auto bytes = map.m_weatherRadarBufferedBytes;
        const auto bounds = map.m_weatherRadarFrameBounds;
        const auto elapsed = map.m_weatherRadarPlaybackCadence.requestedElapsedMs();
        QSignalSpy state(&map, &MapDisplayWidget::weatherRadarAnimationStateChanged);
        // Hidden widget: this state-machine test never requires a GL context.
        map.hide();
        map.setProjectionMode(MapDisplayWidget::ProjectionMode::Globe);
        QCOMPARE(map.projectionMode(), MapDisplayWidget::ProjectionMode::Globe);
        QVERIFY(map.m_weatherRadarPlaybackRequested && map.m_weatherRadarAnimating);
        QCOMPARE(map.m_weatherRadarFrames, frames);
        QCOMPARE(map.m_weatherRadarBufferedBytes, bytes);
        QCOMPARE(map.m_weatherRadarFrameBounds, bounds);
        QCOMPARE(map.m_weatherRadarPlaybackCadence.requestedElapsedMs(), elapsed);
        QCOMPARE(map.weatherRadarRendererBounds(bounds.first()), bounds.first());
        map.setProjectionMode(MapDisplayWidget::ProjectionMode::Flat);
        QVERIFY(state.isEmpty());
        QCOMPARE(map.m_weatherRadarPlaybackSpeedPercent, 137);
        QCOMPARE(map.m_weatherRadarFrameBounds, bounds);
        QCOMPARE(map.weatherRadarRendererBounds(bounds.first()),
            WeatherRadarSource::conventionalBoundsFromQgv(bounds.first()));
        map.m_weatherRadarRebufferTimer->stop();

        map.m_weatherRadarHistoryHours = 1;
        const QDateTime newest = frames.last().addSecs(3600);
        map.appendWeatherRadarObservations({
            {frames.last(), map.m_weatherRadarFrameSampleTimes.last(),
                map.m_weatherRadarFrameRasterIds.last()},
            {newest, newest.addSecs(150), {1000}}});
        QCOMPARE(map.weatherRadarPlaybackFrameCount(), 6);
        QCOMPARE(map.m_weatherRadarBufferedBytes.mid(0, 6), bytes);
        finishAll(map, network);
        QVERIFY(map.m_weatherRadarBufferFinalizationPending);
        // Pruning the reusable cache must not destroy the active frame bytes.
        map.m_weatherRadarFrameCache.clear();
        map.applyFinalizedWeatherRadarBuffering();
        QCOMPARE(map.m_weatherRadarFrames.size(), 2);
        QCOMPARE(map.m_weatherRadarFrames.first(), frames.last());
        QCOMPARE(map.m_weatherRadarFrameBounds.first(), bounds.last());
        QCOMPARE(map.m_weatherRadarBufferedBytes.first(), bytes.last());
        QVERIFY(!map.m_weatherRadarBufferedBytes.last().isEmpty());
        QCOMPARE(map.weatherRadarPlaybackFrameCount(), 2);
        QVERIFY(map.m_weatherRadarAnimating);
        map.stopWeatherRadarAnimation();
        map.setProjectionMode(MapDisplayWidget::ProjectionMode::Globe);
        QVERIFY(!map.m_weatherRadarPlaybackRequested);
        map.setProjectionMode(MapDisplayWidget::ProjectionMode::Flat);
        QVERIFY(!map.m_weatherRadarPlaybackRequested);
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
            map.m_weatherRadarFrames.at(index), map.m_weatherRadarFrameSampleTimes.at(index),
            map.m_weatherRadarFrameRasterIds.at(index));
        const QUrl selectedUrl = source.imageUrl(selected.bounds, selected.size);
        map.m_weatherRadarFrameUrls[index] = selectedUrl;
        map.m_weatherRadarFrameRequestGeometry[index] = selected;
        network.pending()->complete();
        QTRY_VERIFY(network.pending(selectedUrl));
        network.pending(selectedUrl)->complete();
        QTRY_VERIFY(!map.m_weatherRadarBufferedBytes.at(index).isEmpty());
        QCOMPARE(map.m_weatherRadarDecodedImages.value(index).size(), selected.size);
        QCOMPARE(map.m_weatherRadarFrameBounds.at(index), selected.bounds);
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

    void progressiveZoomAndFailures()
    {
        ControlledRadarNetwork network;
        QGV::setNetworkManager(&network);
        MapDisplayWidget map;
        prepare(map, network);
        QSignalSpy frames(&map, &MapDisplayWidget::weatherRadarFrameChanged);
        QCOMPARE(network.requests.size(), 4); // Bounded even on a stalled link.
        map.updateWeatherRadarLoadingStatus();
        QVERIFY(!map.m_weatherRadarLoadingLabel->isVisible());
        QTest::qWait(340);
        map.updateWeatherRadarLoadingStatus();
        QVERIFY(map.m_weatherRadarLoadingLabel->isVisible());
        QCOMPARE(map.m_weatherRadarLoadingLabel->text(), QStringLiteral("Loading radar… 0/6"));
        QVERIFY(map.m_weatherRadarLoadingLabel->testAttribute(Qt::WA_TransparentForMouseEvents));
        QCOMPARE(map.m_weatherRadarLoadingLabel->focusPolicy(), Qt::NoFocus);
        QVERIFY(std::abs(map.m_weatherRadarLoadingLabel->geometry().center().x() - map.width()/2) < 2);

        QVERIFY(network.pending(map.m_weatherRadarFrameUrls.at(0)));
        network.pending(map.m_weatherRadarFrameUrls.at(0))->complete();
        QTRY_VERIFY(map.m_weatherRadarDecodedImages.contains(0));
        QVERIFY(!frames.isEmpty()); // A single complete image is displayed now.
        QVERIFY(!map.m_weatherRadarAnimating);
        network.pending(map.m_weatherRadarFrameUrls.at(1))->complete();
        QTRY_VERIFY(map.m_weatherRadarAnimating); // No five-frame barrier.
        QVERIFY(map.m_weatherRadarPlaybackTimer->isActive());
        QTRY_VERIFY(map.m_weatherRadarFrameIndex == 1);
        finishAll(map, network);
        QVERIFY(map.m_weatherRadarNetworkRequestsComplete);
        QTRY_COMPARE(map.weatherRadarPlaybackFrameCount(), 6);

        QGVMap* flat = map.m_flatView->findChild<QGVMap*>();
        const double originalScale = flat->getCamera().scale();
        const QPointF originalCenter = flat->getCamera().projRect().center();
        const QVector<QRectF> oldBounds = map.m_weatherRadarFrameBounds;
        const QVector<QString> oldKeys = map.m_weatherRadarFrameCacheKeys;
        const qint64 oldClock = map.m_weatherRadarPlaybackClock.elapsed();
        flat->cameraTo(QGVCameraActions(flat).scaleTo(originalScale * 2), false);
        QCoreApplication::processEvents();
        const int beforeRequests = network.requests.size();
        const int displayed = map.m_weatherRadarFrameIndex;
        map.rebufferWeatherRadarPlayback();
        map.m_weatherRadarRebufferTimer->stop();
        QVERIFY(map.m_weatherRadarAnimating);
        QVERIFY(map.m_weatherRadarPlaybackRequested);
        QVERIFY(map.m_weatherRadarPlaybackTimer->isActive());
        QVERIFY(map.m_weatherRadarPlaybackClock.elapsed() >= oldClock);
        QCOMPARE(map.m_weatherRadarFrameBounds, oldBounds);
        QCOMPARE(map.m_weatherRadarFrameCacheKeys, oldKeys);
        QCOMPARE(network.requests.size(), beforeRequests + 4);
        QCOMPARE(network.requests.at(beforeRequests), map.m_weatherRadarFrameUrls.at(displayed));
        QSignalSpy playing(&map, &MapDisplayWidget::weatherRadarAnimationStateChanged);
        QSignalSpy loading(&map, &MapDisplayWidget::weatherRadarTimelineLoadingChanged);
        network.pending(map.m_weatherRadarFrameUrls.at(displayed))->complete();
        QTRY_VERIFY(map.m_weatherRadarFrameCacheKeys.at(displayed) != oldKeys.at(displayed));
        const int neighbor = (displayed + 1) % 6;
        QVERIFY(map.m_weatherRadarFrameBounds.at(displayed) != oldBounds.at(displayed));
        QCOMPARE(map.m_weatherRadarFrameBounds.at(neighbor), oldBounds.at(neighbor));
        QVERIFY(!map.m_weatherRadarDecodedImages.value(displayed).isNull());
        QVERIFY(!map.m_weatherRadarBufferedBytes.at(neighbor).isEmpty());
        QVERIFY(playing.isEmpty() && loading.isEmpty()); // Zoom cannot change Play/Pause.
        const qint64 elapsed = map.m_weatherRadarPlaybackCadence.presentedElapsedMs();
        QTest::qWait(200);
        QVERIFY(map.m_weatherRadarPlaybackCadence.presentedElapsedMs() > elapsed);

        QVERIFY(network.pending());
        network.pending()->complete(true);
        finishAll(map, network);
        QVERIFY(!map.m_weatherRadarDownloadFailed.isEmpty());
        for (const QByteArray& bytes : map.m_weatherRadarBufferedBytes) {
            QVERIFY(!bytes.isEmpty()); // Failure is not "no rain" and not a blank.
        }
        map.updateWeatherRadarLoadingStatus();
        QTest::qWait(340);
        map.updateWeatherRadarLoadingStatus();
        QCOMPARE(map.m_weatherRadarLoadingLabel->text(), QStringLiteral("Loading radar data failed"));
        map.m_weatherRadarRebufferTimer->stop();

        // A late old-generation result must not re-georeference current data.
        const int generation = map.m_weatherRadarBufferGeneration;
        const auto frame = map.m_weatherRadarFrameCache.value(map.m_weatherRadarFrameCacheKeys.at(0));
        const QRectF retained = map.m_weatherRadarFrameBounds.at(0);
        ++map.m_weatherRadarBufferGeneration;
        map.acceptWeatherRadarDownload(generation, 0, oldKeys.at(0), frame);
        QCOMPARE(map.m_weatherRadarFrameBounds.at(0), retained);

        // Returning to the previous view reuses all six original exports.
        flat->cameraTo(QGVCameraActions(flat).scaleTo(originalScale).moveTo(originalCenter), false);
        QCoreApplication::processEvents();
        const int beforeReturn = network.requests.size();
        map.rebufferWeatherRadarPlayback();
        QTRY_VERIFY(map.m_weatherRadarNetworkRequestsComplete);
        QCOMPARE(network.requests.size(), beforeReturn);
        QVERIFY(map.m_weatherRadarDownloadFailed.isEmpty());
        map.m_weatherRadarRebufferTimer->stop();
        flat->cameraTo(QGVCameraActions(flat).moveTo(originalCenter + QPointF(1000, 1000)), false);
        QCoreApplication::processEvents();
        map.rebufferWeatherRadarPlayback();
        QCOMPARE(network.requests.size(), beforeReturn);
        map.stopWeatherRadarAnimation();
        map.m_weatherRadarVisible = false;
        map.updateWeatherRadarLoadingStatus();
        QVERIFY(!map.m_weatherRadarLoadingLabel->isVisible());
        QGV::setNetworkManager(nullptr);
    }
};

} // namespace AetherSDR

QTEST_MAIN(AetherSDR::WeatherRadarLoadingTest)
#include "weather_radar_loading_test.moc"
