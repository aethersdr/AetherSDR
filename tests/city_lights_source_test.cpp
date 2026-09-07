#include "gui/map/CityLightsSource.h"
#include "gui/map/SolarTerminator.h"

#include <QBuffer>
#include <QNetworkReply>
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
        QVERIFY(view.size.width() <= 2048);
        QVERIFY(view.size.height() <= 2048);
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
    }
};

QTEST_GUILESS_MAIN(CityLightsSourceTest)
#include "city_lights_source_test.moc"
