#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVLayerTilesOnline.h>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTest>

// Inject the documented HTTP reply contract. No sockets or provider access.
class TileReply final : public QNetworkReply {
public:
    TileReply(const QNetworkRequest& request, QObject* parent) : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly);
    }
    void abort() override
    {
        if (!isFinished()) {
            finish(OperationCanceledError);
        }
    }
    void finish(NetworkError error)
    {
        setError(error, QStringLiteral("Injected reply"));
        setFinished(true);
        emit finished();
    }
    void reportOversize() { emit downloadProgress(1024 * 1024 + 1, -1); }
protected:
    qint64 readData(char*, qint64) override { return -1; }
};

class TileNetwork final : public QNetworkAccessManager {
public:
    QList<QPointer<TileReply>> replies;
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        auto* reply = new TileReply(request, this);
        replies.append(reply);
        return reply;
    }
};

class OnlineLayer final : public QGVLayerTilesOnline {
public:
    void process() { onUpdate(); }
    void clearLayer() { onClean(); }
protected:
    int minZoomlevel() const override { return 0; }
    int maxZoomlevel() const override { return 0; }
    int scaleToZoom(double) const override { return 0; }
    QString tilePosToUrl(const QGV::GeoTilePos&) const override
    {
        return QStringLiteral("https://example.invalid/tile.png");
    }
};

class MapTileReplyTest final : public QObject {
    Q_OBJECT
private slots:
    void cancellationAndFailure_data()
    {
        QTest::addColumn<int>("action");
        QTest::addColumn<quint64>("failures");
        QTest::newRow("layer-clean") << 0 << quint64(0);
        QTest::newRow("intentional-cancel") << 1 << quint64(0);
        QTest::newRow("size-limit-abort") << 2 << quint64(1);
        QTest::newRow("network-timeout") << 3 << quint64(1);
    }
    void cancellationAndFailure()
    {
        QFETCH(int, action);
        QFETCH(quint64, failures);
        TileNetwork network;
        QGV::setNetworkManager(&network);
        {
            QGVMap map;
            map.resize(400, 300);
            auto* layer = new OnlineLayer();
            map.addItem(layer);
            layer->process();
            QVERIFY(!network.replies.isEmpty());
            TileReply* reply = network.replies.first();
            QVERIFY(reply != nullptr);
            QVERIFY(layer->pendingRequestCount() > 0);
            if (action == 0) {
                layer->clearLayer();
            } else if (action == 1) {
                reply->abort();
            } else if (action == 2) {
                reply->reportOversize();
            } else {
                reply->finish(QNetworkReply::TimeoutError);
            }
            QCOMPARE(layer->failedTileRequestCount(), failures);
            QCOMPARE(layer->pendingRequestCount(), 0);
            // Clearing a completed/failed generation cannot count it again.
            layer->clearLayer();
            QCOMPARE(layer->failedTileRequestCount(), failures);
            // A new generation must still be able to request the same tile.
            const qsizetype previousRequests = network.replies.size();
            layer->process();
            QVERIFY(network.replies.size() > previousRequests);
            layer->clearLayer();
            QCOMPARE(layer->failedTileRequestCount(), failures);
        }
        QGV::setNetworkManager(nullptr);
    }
};

QTEST_MAIN(MapTileReplyTest)
#include "map_tile_reply_test.moc"
