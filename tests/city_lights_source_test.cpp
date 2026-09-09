#include "gui/map/CityLightsShading.h"
#include "gui/map/CityLightsSource.h"
#include "gui/map/MapProviderNetworkAccessManager.h"
#include "gui/map/SolarTerminator.h"

#include <QBuffer>
#include <QNetworkReply>
#include <QNetworkDiskCache>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTest>
#include <QUrlQuery>

#include <cstring>
#include <limits>

using namespace AetherSDR;

class LightsReply final : public QNetworkReply {
public:
    LightsReply(const QNetworkRequest& request, QObject* parent) : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    void abort() override
    {
        if (!isFinished()) {
            setError(OperationCanceledError, "Canceled");
            setFinished(true);
            emit finished();
        }
    }
    void complete(bool fail = false)
    {
        if (isFinished()) {
            return;
        }
        if (fail) {
            setError(TimeoutError, "Injected timeout");
        } else {
            const QUrlQuery query(url());
            QImage image(query.queryItemValue("WIDTH").toInt(),
                         query.queryItemValue("HEIGHT").toInt(), QImage::Format_ARGB32);
            image.fill(Qt::white);
            QBuffer buffer(&m_bytes);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
            emit readyRead();
        }
        setFinished(true);
        emit finished();
    }
    void completeInvalidImage()
    {
        m_bytes = "not a PNG";
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        setFinished(true);
        emit readyRead();
        emit finished();
    }
    qint64 bytesAvailable() const override { return m_bytes.size() + QIODevice::bytesAvailable(); }
protected:
    qint64 readData(char* data, qint64 size) override
    {
        const qint64 count = std::min(size, qint64(m_bytes.size()));
        std::memcpy(data, m_bytes.constData(), size_t(count));
        m_bytes.remove(0, count);
        return count;
    }
private:
    QByteArray m_bytes;
};

class LightsNetwork final : public QNetworkAccessManager {
public:
    QList<QPointer<LightsReply>> requests;
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        auto* reply = new LightsReply(request, this);
        requests.append(reply);
        return reply;
    }
};

class CityLightsSourceTest final : public QObject {
    Q_OBJECT
private slots:
    void requestGeometryAndDataset()
    {
        const auto view = CityLightsSource::boundedView({QRectF(-1e7, 2e6, 2e6, 3e6), QSize(12000, 12000)});
        QVERIFY(view.size.width() <= 4096);
        QVERIFY(view.size.height() <= 4096);
        QVERIFY(view.size.width() > 2048);
        QVERIFY(view.size.height() > 2048);
        QVERIFY(view.bounds.contains(QRectF(-1e7, 2e6, 2e6, 3e6)));
        const QUrl url = CityLightsSource::imageUrl(view);
        const QUrlQuery query(url);
        QCOMPARE(url.host(), "gibs.earthdata.nasa.gov");
        QCOMPARE(query.queryItemValue("LAYERS"), "VIIRS_Night_Lights");
        QCOMPARE(query.queryItemValue("TIME"), "2016-01-01");
        QCOMPARE(query.queryItemValue("SRS"), "EPSG:3857");
        const auto close = CityLightsSource::boundedView({QRectF(0, 0, 1000, 1000), QSize(2048, 2048)});
        QVERIFY(close.bounds.width() / close.size.width() >= kRadarWorldWidth / 65536.0);
        QVERIFY(CityLightsSource::boundedView({QRectF(0, 0, 1e30, 1), QSize(100, 100)}).bounds.isEmpty());
        QVERIFY(CityLightsSource::boundedView({QRectF(0, 0, std::numeric_limits<double>::infinity(), 1), QSize(100, 100)}).bounds.isEmpty());
    }

    void blackBackgroundAndPremultipliedEdges()
    {
        QImage original(3, 1, QImage::Format_ARGB32);
        original.setPixel(0, 0, qRgba(0, 0, 0, 255));
        original.setPixel(1, 0, qRgba(100, 80, 20, 255));
        original.setPixel(2, 0, qRgba(255, 255, 255, 128));
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(original.save(&buffer, "PNG"));
        const QImage image = CityLightsSource::decode(bytes, original.size());
        QVERIFY(!image.isNull());
        QCOMPARE(image.format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(qAlpha(image.pixel(0, 0)), 0);
        QCOMPARE(image.pixel(1, 0), qRgba(100, 80, 20, 100));
        QCOMPARE(image.pixel(2, 0), qRgba(128, 128, 128, 128));
        QVERIFY(CityLightsSource::decode(bytes, QSize(20, 20)).isNull());
        QVERIFY(CityLightsSource::decode("<ServiceException>unavailable</ServiceException>", QSize(3, 1)).isNull());
        QVERIFY(CityLightsSource::decode(bytes, QSize(30000, 30000)).isNull());
    }

    void faintLightsPreserveBlackHighlightsAndHue()
    {
        QImage input(256, 1, QImage::Format_ARGB32_Premultiplied);
        for (int x = 0; x < 256; ++x) {
            input.setPixel(x, 0, qRgba(x, x / 2, 0, x));
        }
        const QRectF bounds(0, 0, 1000, 1000);
        const QDateTime time = QDateTime::fromString("2026-03-20T12:00:00Z", Qt::ISODate);
        QCOMPARE(CityLightsSource::nightImage(input, bounds, time, false, 0), input);
        const QImage enhanced = CityLightsSource::nightImage(input, bounds, time, false, 50);
        QCOMPARE(enhanced.pixel(0, 0), input.pixel(0, 0));
        QCOMPARE(enhanced.pixel(255, 0), input.pixel(255, 0));
        QVERIFY(qRed(enhanced.pixel(16, 0)) > 2 * 16);
        for (int x = 1; x < 256; ++x) {
            const QRgb p = enhanced.pixel(x, 0);
            QVERIFY(qRed(p) >= qRed(enhanced.pixel(x - 1, 0)));
            QCOMPARE(qRed(p), qAlpha(p));
            QVERIFY(qGreen(p) <= qAlpha(p));
            QCOMPARE(qBlue(p), 0);
        }
        const QImage maximum = CityLightsSource::nightImage(input, bounds, time, false, 100);
        QVERIFY(qRed(maximum.pixel(16, 0)) > qRed(enhanced.pixel(16, 0)));
    }

    void warmthPreservesCoverage()
    {
        QImage input(2, 1, QImage::Format_ARGB32_Premultiplied);
        input.setPixel(0, 0, qRgba(255, 255, 255, 255));
        input.setPixel(1, 0, 0);
        const QImage warm = CityLightsSource::nightImage(input, QRectF(0, 0, 100, 100),
            QDateTime::currentDateTimeUtc(), false, 0, 25);
        QCOMPARE(warm.pixel(0, 0), qRgba(255, 245, 230, 255));
        QCOMPARE(warm.pixel(1, 0), QRgb(0));
        // The CPU path and the globe shader share one set of constants; a
        // change on either side must show up here and in the shader source.
        QCOMPARE(warm.pixel(0, 0), qRgba(255,
            qRound(255 * (1.0 - CityLightsShading::kWarmthGreenLoss * 0.25)),
            qRound(255 * (1.0 - CityLightsShading::kWarmthBlueLoss * 0.25)), 255));
    }

    void shaderCompiledFromSharedConstants()
    {
        const QString shader = CityLightsShading::fragmentShaderSource();
        QVERIFY(!shader.contains(QLatin1String("WARMTH_")));
        QVERIFY(!shader.contains(QLatin1String("TWILIGHT_SINE")));
        QVERIFY(shader.contains(QStringLiteral("1.0 - %1 * warmth, 1.0 - %2 * warmth")
            .arg(CityLightsShading::glslNumber(CityLightsShading::kWarmthGreenLoss),
                 CityLightsShading::glslNumber(CityLightsShading::kWarmthBlueLoss))));
        QVERIFY(shader.contains(QStringLiteral("/ %1, 0.0, 1.0)")
            .arg(CityLightsShading::glslNumber(CityLightsShading::twilightSine()))));
        QCOMPARE(CityLightsShading::faintLightsGamma(0), 1.0);
        QCOMPARE(CityLightsShading::faintLightsGamma(100), 1.0 - CityLightsShading::kFaintLightsGammaSpan);
        QCOMPARE(CityLightsShading::faintLightsGamma(250), CityLightsShading::faintLightsGamma(100));
        QVERIFY(std::abs(CityLightsShading::twilightSine() - std::sin(M_PI / 30.0)) < 1e-12);
    }

    void solarMaskUsesNorthPositiveBounds()
    {
        const QDateTime time = QDateTime::fromString("2026-03-20T12:00:00Z", Qt::ISODate);
        const double sun = qRadiansToDegrees(SolarTerminator::positionAt(time).subsolarLonRad);
        QImage white(1, 1, QImage::Format_ARGB32_Premultiplied);
        white.fill(Qt::white);
        const auto pixelAt = [&](double lat, double lon) {
            const double x = lon / 180 * kRadarMercatorExtent;
            const double y = std::asinh(std::tan(lat * M_PI / 180)) / M_PI * kRadarMercatorExtent;
            return CityLightsSource::nightImage(white, QRectF(x - 1, y - 1, 2, 2), time, true).pixel(0, 0);
        };
        QCOMPARE(qAlpha(pixelAt(0, sun)), 0);
        QCOMPARE(qAlpha(pixelAt(0, sun + 180)), 255);
        const int twilight = qAlpha(pixelAt(0, sun + 93));
        QVERIFY(twilight > 80 && twilight < 180);
        QCOMPARE(CityLightsSource::nightImage(white, QRectF(0, 0, 10, 10), time, false), white);
        const QDateTime summer = QDateTime::fromString("2026-06-21T12:00:00Z", Qt::ISODate);
        const auto polar = [&](double northing) {
            return qAlpha(CityLightsSource::nightImage(white,
                QRectF(-1, northing - 1, 2, 2), summer, true).pixel(0, 0));
        };
        QCOMPARE(polar(1.9e7), 0);
        QCOMPARE(polar(-1.9e7), 255);
    }

    void transportFailurePreservesCacheButInvalidPayloadIsEvicted()
    {
        LightsNetwork network;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto* cache = new QNetworkDiskCache(&network);
        cache->setCacheDirectory(directory.path());
        network.setCache(cache);
        const WeatherRadarViewGeometry view{QRectF(-1e7, 2e6, 1e6, 1e6), QSize(32, 32)};
        const auto bounded = CityLightsSource::boundedView(view);
        const QUrl url = CityLightsSource::imageUrl(bounded);
        QNetworkCacheMetaData metadata;
        metadata.setUrl(url);
        metadata.setExpirationDate(QDateTime::currentDateTimeUtc().addDays(1));
        metadata.setSaveToDisk(true);
        QIODevice* data = cache->prepare(metadata);
        QVERIFY(data != nullptr);
        QImage image(bounded.size, QImage::Format_ARGB32);
        image.fill(Qt::white);
        QVERIFY(image.save(data, "PNG"));
        cache->insert(data);
        CityLightsSource source(nullptr, &network);
        source.setView(view);
        source.setEnabled(true);
        QTRY_COMPARE(network.requests.size(), 1);
        QSignalSpy statuses(&source, &CityLightsSource::statusChanged);
        network.requests.last()->complete(true);
        QTRY_VERIFY(!statuses.isEmpty());
        QVERIFY(cache->metaData(url).isValid());
        source.setEnabled(false);
        source.setEnabled(true);
        QTRY_COMPARE(network.requests.size(), 2);
        // A successful HTTP response with invalid image bytes must still
        // evict its payload.
        network.requests.last()->completeInvalidImage();
        statuses.clear();
        QTRY_VERIFY(!cache->metaData(url).isValid());
    }

    void loadingCancellationRetentionAndReuse()
    {
        LightsNetwork network;
        CityLightsSource source(nullptr, &network);
        source.setNightOnly(false);
        source.setView({QRectF(-1e7, 2e6, 1e6, 1e6), QSize(32, 32)});
        QTest::qWait(450);
        QCOMPARE(network.requests.size(), 0); // Opt-in only.
        QSignalSpy changed(&source, &CityLightsSource::imageChanged);
        QSignalSpy status(&source, &CityLightsSource::statusChanged);
        source.setEnabled(true);
        QTRY_COMPARE(network.requests.size(), 1);
        network.requests.last()->complete();
        QTRY_COMPARE(changed.size(), 1);
        const QImage retained = source.image();
        const QRectF retainedBounds = source.bounds();
        source.setView({QRectF(-1e7 + 100, 2e6 + 100, 999000, 999000), QSize(32, 32)});
        QTest::qWait(500);
        QCOMPARE(network.requests.size(), 1);
        source.setView({QRectF(5e6, -4e6, 1e6, 1e6), QSize(32, 32)});
        QTRY_COMPARE(network.requests.size(), 2);
        QCOMPARE(source.image(), retained);
        QCOMPARE(source.bounds(), retainedBounds);
        network.requests.last()->complete(true);
        QTRY_VERIFY(status.last().at(0).toString().contains("unavailable"));
        QCOMPARE(source.image(), retained);
        QCOMPARE(source.bounds(), retainedBounds);
        source.setView({QRectF(8e6, -4e6, 1e6, 1e6), QSize(32, 32)});
        QTRY_COMPARE(network.requests.size(), 3);
        const QPointer<LightsReply> pending = network.requests.last();
        source.setEnabled(false);
        QVERIFY(pending == nullptr || pending->isFinished());
        QTest::qWait(100);
        QCOMPARE(changed.size(), 1);
        source.setEnabled(true);
        QTRY_COMPARE(network.requests.size(), 4);
        network.requests.last()->complete();
        QTRY_VERIFY(source.bounds() != retainedBounds);
        QTRY_VERIFY(!source.renderPending());
        // A parameter change re-renders asynchronously; consumers must not
        // present image() until imageChanged says it reflects the parameters.
        const int before = changed.size();
        source.setFaintLights(10);
        QVERIFY(source.renderPending());
        // Queued behind the running render: that render's result is stale and
        // is dropped, so exactly one imageChanged follows for the final state.
        source.setWarmth(40);
        QVERIFY(source.renderPending());
        QTRY_COMPARE(changed.size(), before + 1);
        QVERIFY(!source.renderPending());
        QTest::qWait(100);
        QCOMPARE(changed.size(), before + 1);
        source.setWarmth(40); // Unchanged value schedules nothing.
        QVERIFY(!source.renderPending());
    }
};

QTEST_GUILESS_MAIN(CityLightsSourceTest)
#include "city_lights_source_test.moc"
