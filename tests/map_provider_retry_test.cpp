#include "gui/map/MapProviderNetworkAccessManager.h"

#include <QNetworkReply>
#include <QNetworkDiskCache>
#include <QTemporaryDir>
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
// Qt may only execute cache-only requests in this fixture. Even a broken guard
// cannot contact a provider: any attempted wire operation becomes an inert reply.
class CacheOnlyNetwork final : public MapProviderNetworkAccessManager {
public:
    using MapProviderNetworkAccessManager::MapProviderNetworkAccessManager;
    int wireAttempts{0};
protected:
    QNetworkReply* sendRequest(Operation operation, const QNetworkRequest& request,
                              QIODevice* data) override
    {
        if (request.attribute(QNetworkRequest::CacheLoadControlAttribute).toInt()
            != QNetworkRequest::AlwaysCache) {
            ++wireAttempts;
            return new Reply(request, this);
        }
        return MapProviderNetworkAccessManager::sendRequest(operation, request, data);
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
        QCOMPARE(MapProviderRetryPolicy::retryAfterMs("99999999999999999999999999999", now), 86400000);
        QCOMPARE(MapProviderRetryPolicy::retryAfterMs("86401", now), 86400000);
        QCOMPARE(MapProviderRetryPolicy::retryAfterMs("Wed, 09 Sep 2026 12:00:00 GMT", now), 86400000);
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

    void simultaneousFailuresCountOnceAndConsumerTimerCoversJitter()
    {
        qint64 time = 0;
        MapProviderRetryPolicy policy([&time] { return time; });
        QList<MapProviderRetryPolicy::Admission> admitted;
        for (int i = 0; i < 20; ++i) {
            admitted.append(policy.admit(nws));
        }
        for (const auto admission : admitted) {
            policy.complete(nws, admission, true, false);
        }
        const qint64 delay = policy.admit(nws).delayMs;
        QVERIFY(delay >= 60000 && delay <= 66000);
        QVERIFY(MapProviderRetryPolicy::kConsumerRetryMs >= 66000);
        time = MapProviderRetryPolicy::kConsumerRetryMs;
        const auto recovery = policy.admit(nws);
        QCOMPARE(recovery.delayMs, 0);
        QVERIFY(recovery.probe);
    }

    void lateServerDeadlineIsHonoredWithoutMultiplyingBackoff()
    {
        qint64 time = 0;
        MapProviderRetryPolicy policy([&time] { return time; });
        const auto first = policy.admit(nws);
        const auto late = policy.admit(nws);
        policy.complete(nws, first, true, false);
        time = 10000;
        policy.complete(nws, late, true, false, "3600");
        QCOMPARE(policy.admit(nws).delayMs, 3600000);
        time += 3600000;
        const auto probe = policy.admit(nws);
        policy.complete(nws, probe, true, false);
        const qint64 delay = policy.admit(nws).delayMs;
        QVERIFY(delay >= 120000 && delay <= 126000);
    }

    void diskCacheRemainsAvailableDuringCooldown()
    {
        qint64 time = 0;
        auto policy = std::make_shared<MapProviderRetryPolicy>([&time] { return time; });
        CacheOnlyNetwork network(nullptr, policy);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto* cache = new QNetworkDiskCache(&network);
        cache->setCacheDirectory(directory.path());
        network.setCache(cache);
        QNetworkCacheMetaData metadata;
        metadata.setUrl(nws);
        metadata.setSaveToDisk(true);
        metadata.setExpirationDate(QDateTime::currentDateTimeUtc().addDays(1));
        metadata.setRawHeaders({{"Content-Type", "image/png"}, {"Cache-Control", "max-age=86400"}});
        QIODevice* data = cache->prepare(metadata);
        QVERIFY(data != nullptr);
        data->write("cached-image");
        cache->insert(data);
        const auto admitted = policy->admit(nws);
        policy->complete(nws, admitted, true, false, "180");
        for (const auto mode : {QNetworkRequest::PreferCache, QNetworkRequest::AlwaysCache}) {
            QNetworkRequest request(nws);
            request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, mode);
            QNetworkReply* reply = network.get(request);
            QSignalSpy done(reply, &QNetworkReply::finished);
            QTRY_COMPARE(done.size(), 1);
            QCOMPARE(reply->error(), QNetworkReply::NoError);
            QVERIFY(reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool());
            QCOMPARE(reply->readAll(), QByteArray("cached-image"));
            QCOMPARE(policy->admit(nws).delayMs, 180000);
            delete reply;
        }
        // A miss and an explicit network-only load are denied locally.
        for (const auto mode : {QNetworkRequest::PreferCache, QNetworkRequest::AlwaysNetwork}) {
            QNetworkRequest request(mode == QNetworkRequest::AlwaysNetwork ? nws
                : QUrl(nws.toString() + "/missing"));
            request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, mode);
            QNetworkReply* reply = network.get(request);
            QSignalSpy done(reply, &QNetworkReply::finished);
            QTRY_COMPARE(done.size(), 1);
            QVERIFY(reply->error() != QNetworkReply::NoError);
            QCOMPARE(policy->admit(nws).delayMs, 180000);
            delete reply;
        }
        QCOMPARE(network.wireAttempts, 0);
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
