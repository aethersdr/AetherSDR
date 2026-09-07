#include "gui/map/MapProviderNetworkAccessManager.h"

#include <QNetworkReply>
#include <QSignalSpy>
#include <QTest>

using namespace AetherSDR;
namespace {
const QUrl nasa("https://gibs.earthdata.nasa.gov/wms/test");
const QUrl nws("https://mapservices.weather.noaa.gov/eventdriven/test");

class Reply final : public QNetworkReply {
public:
    Reply(const QNetworkRequest& request, QObject* parent) : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly);
    }
    void abort() override
    {
        if (!isFinished()) {
            setError(OperationCanceledError, "Canceled");
            setFinished(true);
            emit finished();
        }
    }
    void finish(int status, const QByteArray& retryAfter = {}, bool cached = false)
    {
        setAttribute(QNetworkRequest::SourceIsFromCacheAttribute, cached);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        setRawHeader("Retry-After", retryAfter);
        if (status >= 400) {
            setError(TemporaryNetworkFailureError, "Injected provider failure");
        }
        setFinished(true);
        emit finished();
    }
protected:
    qint64 readData(char*, qint64) override { return -1; }
};
class Network final : public MapProviderNetworkAccessManager {
public:
    using MapProviderNetworkAccessManager::MapProviderNetworkAccessManager;
    QList<Reply*> sent;
protected:
    QNetworkReply* sendRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        auto* reply = new Reply(request, this);
        sent.append(reply);
        return reply;
    }
};
}

class MapProviderRetryTest final : public QObject {
    Q_OBJECT
private slots:
    void parsesRetryAfterWithoutOverflow()
    {
        const QDateTime now = QDateTime::fromString("2026-09-07T12:00:00Z", Qt::ISODate);
        QCOMPARE(MapProviderRetryPolicy::retryAfterMs("120", now), 120000);
        QCOMPARE(MapProviderRetryPolicy::retryAfterMs("Mon, 07 Sep 2026 12:03:00 GMT", now), 180000);
        QCOMPARE(MapProviderRetryPolicy::retryAfterMs("Mon, 07 Sep 2026 11:59:00 GMT", now), 0);
        for (const QByteArray text : {QByteArray(), QByteArray("-1"), QByteArray("broken"), QByteArray("1.5")}) {
            QCOMPARE(MapProviderRetryPolicy::retryAfterMs(text, now), 0);
        }
        QVERIFY(MapProviderRetryPolicy::retryAfterMs("99999999999999999999999999999", now) > 86400000);
    }

    void cooldownEscalatesAndOldSuccessCannotClearIt()
    {
        qint64 time = 0;
        MapProviderRetryPolicy policy([&time] { return time; });
        const auto first = policy.admit(nws);
        const auto older = policy.admit(nws);
        policy.complete(nws, first, true, false);
        qint64 delay = policy.admit(nws).delayMs;
        QVERIFY(delay >= 60000 && delay <= 66000);
        policy.complete(nws, older, false, false);
        QCOMPARE(policy.admit(nws).delayMs, delay);
        time += delay;
        const auto probe = policy.admit(nws);
        QCOMPARE(probe.delayMs, 0);
        QVERIFY(probe.probe);
        QVERIFY(policy.admit(nws).delayMs > 0); // One recovery request across all views.
        policy.complete(nws, probe, true, false);
        delay = policy.admit(nws).delayMs;
        QVERIFY(delay >= 120000 && delay <= 126000);
        time += delay;
        const auto recovered = policy.admit(nws);
        policy.complete(nws, recovered, false, false);
        const auto fresh = policy.admit(nws);
        QVERIFY(!fresh.probe);
        policy.complete(nws, fresh, true, false);
        QVERIFY(policy.admit(nws).delayMs >= 60000 && policy.admit(nws).delayMs <= 66000);
    }

    void lateFailureCannotStrandAnOlderProbe()
    {
        qint64 time = 0;
        MapProviderRetryPolicy policy([&time] { return time; });
        const auto first = policy.admit(nws);
        const auto late = policy.admit(nws);
        policy.complete(nws, first, true, false);
        time += policy.admit(nws).delayMs;
        const auto probe = policy.admit(nws);
        policy.complete(nws, late, true, false);
        policy.complete(nws, probe, false, false);
        time += policy.admit(nws).delayMs;
        const auto next = policy.admit(nws);
        QCOMPARE(next.delayMs, 0);
        QVERIFY(next.probe);
    }

    void retryAfterWinsAndProvidersAreIndependent()
    {
        qint64 time = 0;
        MapProviderRetryPolicy policy([&time] { return time; });
        const auto first = policy.admit(nasa);
        policy.complete(nasa, first, true, false, "3600");
        QCOMPARE(policy.admit(nasa).delayMs, 3600000);
        QCOMPARE(policy.admit(QUrl("https://gibs-a.earthdata.nasa.gov/other")).delayMs, 3600000);
        QCOMPARE(policy.admit(nws).delayMs, 0);
        QCOMPARE(policy.admit(QUrl("https://gibs.earthdata.nasa.gov.evil.example/")).delayMs, 0);
        time += 3599999;
        QCOMPARE(policy.admit(nasa).delayMs, 1);
    }

    void networkManagersShareCooldownAndCancelWithoutRequests()
    {
        qint64 time = 0;
        auto policy = std::make_shared<MapProviderRetryPolicy>([&time] { return time; });
        Network flat(nullptr, policy);
        Network globe(nullptr, policy);
        flat.get(QNetworkRequest(nws));
        QCOMPARE(flat.sent.size(), 1);
        flat.sent.last()->finish(429, "180");
        QNetworkReply* denied = globe.get(QNetworkRequest(QUrl(nws.toString() + "/new-viewport")));
        QSignalSpy done(denied, &QNetworkReply::finished);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(globe.sent.size(), 0);
        QCOMPARE(denied->rawHeader("Retry-After"), QByteArray("180"));
        QVERIFY(denied->error() != QNetworkReply::NoError);
        // Local denials do not slide the deadline on every UI retry.
        QCOMPARE(policy->admit(nws).delayMs, 180000);
        QNetworkReply* canceled = globe.get(QNetworkRequest(nws));
        QSignalSpy canceledDone(canceled, &QNetworkReply::finished);
        canceled->abort();
        QTest::qWait(10);
        QCOMPARE(canceledDone.size(), 1);
        QCOMPARE(canceled->error(), QNetworkReply::OperationCanceledError);
        QCOMPARE(globe.sent.size(), 0);
        globe.get(QNetworkRequest(QUrl("https://tile.openstreetmap.org/0/0/0.png")));
        QCOMPARE(globe.sent.size(), 1); // Other services retain their normal behavior.
        time = 180000;
        globe.get(QNetworkRequest(nws));
        QCOMPARE(globe.sent.size(), 2);
        flat.get(QNetworkRequest(nws));
        QCOMPARE(flat.sent.size(), 1); // A second projection cannot race the probe.
        globe.sent.last()->finish(200);
        flat.get(QNetworkRequest(nws));
        QCOMPARE(flat.sent.size(), 2);
    }

    void backoffCapsAndCachedSuccessDoesNotReleaseRecovery()
    {
        qint64 time = 0;
        auto policy = std::make_shared<MapProviderRetryPolicy>([&time] { return time; });
        Network network(nullptr, policy);
        for (int attempt = 0; attempt < 8; ++attempt) {
            network.get(QNetworkRequest(nws));
            network.sent.last()->finish(503);
            const qint64 delay = policy->admit(nws).delayMs;
            QVERIFY(delay >= 60000 && delay <= 906000);
            if (attempt >= 4) {
                QVERIFY(delay >= 900000);
            }
            time += delay;
        }
        network.get(QNetworkRequest(nws));
        network.sent.last()->finish(200, {}, true);
        network.get(QNetworkRequest(nws)); // Still a single recovery probe.
        QVERIFY(policy->admit(nws).delayMs > 0);
        network.sent.last()->finish(200);
        QVERIFY(!policy->admit(nws).probe);
    }

    void canceledAndDestroyedProbeCanBeRetried()
    {
        qint64 time = 0;
        auto policy = std::make_shared<MapProviderRetryPolicy>([&time] { return time; });
        {
            Network network(nullptr, policy);
            network.get(QNetworkRequest(nasa));
            network.sent.last()->finish(503, "60");
            time += policy->admit(nasa).delayMs;
            network.get(QNetworkRequest(nasa));
            QVERIFY(policy->admit(nasa).delayMs > 0);
        }
        Network reopened(nullptr, policy);
        QNetworkReply* probe = reopened.get(QNetworkRequest(nasa));
        QCOMPARE(reopened.sent.size(), 1);
        probe->abort();
        reopened.get(QNetworkRequest(nasa));
        QCOMPARE(reopened.sent.size(), 2);
        reopened.sent.last()->finish(200);
    }
};
QTEST_GUILESS_MAIN(MapProviderRetryTest)
#include "map_provider_retry_test.moc"
