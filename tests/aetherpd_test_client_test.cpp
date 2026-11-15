#include "AetherPDTestNetwork.h"
#include "core/txlinearity/AetherPDTestClient.h"

#include <QByteArray>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <memory>
#include <limits>

using AetherSDR::txlin::AetherPDTestClient;
namespace pd = AetherSDR::aetherpd;

namespace {
const QString kId = QStringLiteral("0123456789abcdef0123456789abcdef");
const QString kToken(43, u'a');
QByteArray observation(int remaining, const QString& state = QStringLiteral("observing"),
                       const QString& id = kId, const QString& due = QStringLiteral("20000"))
{
    return QJsonDocument(QJsonObject{{"test_id", id}, {"state", state},
        {"cleanup_due_ms", due}, {"remaining_ms", remaining}}).toJson(QJsonDocument::Compact);
}
struct Fixture {
    Network network;
    qint64 now{1000};
    bool current{true};
    int changes{0};
    AetherPDTestClient client{network, [this] { return now; }};
    Fixture()
    {
        client.setSession(QHostAddress(QStringLiteral("192.168.50.100")), kToken, [this] { return current; });
        client.setChangedCallback([this] { ++changes; });
    }
    bool ready()
    {
        if (!client.begin(15000)) { return false; }
        now += 2000;
        network.last->complete(200, observation(15000));
        if (client.state() != AetherPDTestClient::State::Verifying || network.sent.size() != 2) { return false; }
        now += 10;
        network.last->complete(200, observation(14990));
        return client.state() == AetherPDTestClient::State::Observing;
    }
};
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    ok &= check(pd::testBeginBody(15000) == QByteArray("{\"cleanup_after_ms\":15000}"), "canonical begin body");
    ok &= check(!pd::testBeginBody(0) && !pd::testBeginBody(15001), "duration bounds");
    ok &= check(!pd::testRequest(pd::TestOperation::Stop, QString(32, u'0'))
        && !pd::testRequest(pd::TestOperation::Begin, kId), "request identity bounds");
    const QByteArray valid = observation(1000);
    ok &= check(bool(pd::parseTestObservation(valid)), "valid protocol observation");
    for (const QByteArray& body : {valid + "{}", QByteArray(valid).replace("\"state\":", "\"state\":\"ended\",\"state\":"),
             QByteArray(valid).replace("\"test_id\":", "\"test_id\":\"x\",\"test_\\u0069d\":"),
             observation(0), observation(15001), observation(1, QStringLiteral("ended")),
             observation(1, QStringLiteral("observing"), kId, QStringLiteral("020000")),
             QByteArray(valid).replace("1000", "1e3"), QByteArray(valid).replace("1000", "null")}) {
        ok &= check(!pd::parseTestObservation(body), "invalid protocol observation refused");
    }
    {
        Network network;
        qint64 now = 1000;
        bool destroy = false;
        std::unique_ptr<AetherPDTestClient> client = std::make_unique<AetherPDTestClient>(network, [&] { return now; });
        client->setSession(QHostAddress(QStringLiteral("192.168.50.100")), kToken, [] { return true; });
        client->setChangedCallback([&] { if (destroy) { client.reset(); } });
        ok &= check(client->begin(15000), "cleanup callback fixture starts");
        network.last->complete(200, observation(15000));
        network.last->complete(200, observation(15000));
        destroy = true; now += 500;
        ok &= check(client->cleanupRemainingUpperMs() == 0 && !client,
            "cleanup estimate tolerates destruction during stale-observation notification");
    }
    {
        Network network;
        AetherPDTestClient client(network, [] { return std::numeric_limits<qint64>::max() - 15000; });
        client.setSession(QHostAddress(QStringLiteral("192.168.50.100")), kToken, [] { return true; });
        ok &= check(!client.begin(15000) && network.sent.empty(),
            "cleanup estimate reserves quantization headroom before request admission");
    }
    {
        Fixture f;
        ok &= check(f.ready(), "cleanup estimate fixture ready");
        f.now = 3250; f.client.poll();
        f.now = 3450; f.network.last->complete(200, observation(14740));
        ok &= check(f.client.cleanupRemainingUpperMs() == 14551,
            "longer later RTT cannot push cleanup upper estimate into the future");
        f.now = 3750;
        ok &= check(f.client.cleanupRemainingUpperMs() == 0,
            "stale observations cannot supply cleanup timing even before polling");
    }
    {
        Fixture f;
        ok &= check(f.ready(), "separate fresh verification establishes availability");
        ok &= check(f.network.sent[0].method == QNetworkAccessManager::PostOperation
            && f.network.sent[0].body == QByteArray("{\"cleanup_after_ms\":15000}")
            && f.network.sent[0].request.rawHeader("Authorization") == "Bearer " + kToken.toLatin1()
            && f.network.sent[1].request.url().path() == "/v1/aetherpd/test-unkey/" + kId,
            "fixed authenticated requests");
        ok &= check(f.client.remainingMs() == 12990, "startup and RTT deducted from fixed budget");
        ok &= check(f.client.cleanupRemainingUpperMs() == 14991,
            "cleanup upper estimate retains startup/roundtrip uncertainty plus quantization");
        f.now = 3250; f.client.poll();
        f.now = 3260; f.network.last->complete(200, observation(14740));
        ok &= check(f.client.remainingMs() == 12740, "later status cannot extend initial local deadline");
        ok &= check(f.client.cleanupRemainingUpperMs() == 14741,
            "later observation cannot extend cleanup upper deadline");
        ok &= check(!f.client.begin(15000) && !f.client.setSession({}, {}, {}), "test cannot rearm or rebind");
        f.now = 3500; f.client.poll();
        QPointer<Reply> old = f.network.last;
        ok &= check(f.client.requestStop() && old->aborted, "stop withdraws availability and cancels read");
        ok &= check(f.client.remainingMs() == 0 && f.client.cleanupRemainingUpperMs() == 0
            && f.network.sent.back().request.url().path().endsWith("/" + kId + "/stop"), "stop retains original ID");
        old->complete(200, observation(14500));
        ok &= check(f.client.state() == AetherPDTestClient::State::Retiring, "late read cannot resurrect stopped test");
        f.network.last->complete(202, observation(0, QStringLiteral("retiring")));
        ok &= check(f.client.state() == AetherPDTestClient::State::Retiring, "stop ACK is not helper termination");
        f.now = 3750; f.client.poll();
        f.network.last->complete(200, observation(0, QStringLiteral("ended")));
        ok &= check(f.client.state() == AetherPDTestClient::State::Ended && f.client.remainingMs() == 0, "helper ended without transmit permission");
    }
    for (int fault = 0; fault < 7; ++fault) {
        Fixture f;
        ok &= check(f.ready(), "fault fixture ready");
        f.now = 3250; f.client.poll();
        if (fault == 0) { f.current = false; }
        if (fault == 1) { f.now = 2999; }
        if (fault == 2) { f.now = 3750; }
        QByteArray body = observation(14740);
        if (fault == 3) { body = observation(14740, QStringLiteral("observing"), QString(32, u'a')); }
        if (fault == 4) { body = observation(14740, QStringLiteral("observing"), kId, QStringLiteral("20001")); }
        if (fault == 5) { body = observation(14991); }
        if (fault == 6) { body = observation(0, QStringLiteral("ended")); }
        f.network.last->complete(200, body);
        ok &= check(f.client.state() == AetherPDTestClient::State::Failed && f.client.remainingMs() == 0, "fault permanently withdraws availability");
        f.current = true; f.now = 4000; f.client.poll();
        ok &= check(f.client.remainingMs() == 0 && f.network.sent.size() == 3, "no automatic recovery or retry");
    }
    {
        Fixture f;
        ok &= check(f.ready(), "freshness fixture ready");
        f.now = 3500;
        ok &= check(f.client.remainingMs() == 0 && f.client.state() == AetherPDTestClient::State::Failed, "read gate rejects stale status without timer delivery");
    }
    {
        Fixture f;
        f.client.begin(1000);
        f.now = 2100;
        f.network.last->complete(200, observation(1000));
        ok &= check(f.client.state() == AetherPDTestClient::State::Failed && f.network.sent.size() == 1, "late begin cannot verify or key");
        ok &= check(f.client.requestStop(), "late begin retains ID for narrow cleanup");
    }
    {
        Fixture f;
        f.client.begin(15000);
        QPointer<Reply> pending = f.network.last;
        f.client.clearSession();
        ok &= check(pending->aborted && f.client.state() == AetherPDTestClient::State::Failed, "clear cancels unidentified begin");
        pending->complete(200, valid);
        ok &= check(f.client.testId().isEmpty() && f.client.remainingMs() == 0, "late abandoned begin ignored");
    }
    {
        Fixture f;
        f.client.begin(15000); f.current = false;
        f.network.last->complete(200, observation(15000));
        ok &= check(f.client.state() == AetherPDTestClient::State::Failed && f.client.requestStop(), "lost owner retains only original-ID cleanup");
        ok &= check(f.network.sent.back().request.url().path().endsWith("/" + kId + "/stop"), "cleanup cannot target a new test");
    }
    for (int code : {401, 302, 409, 503}) {
        Fixture f; f.client.begin(15000); f.network.last->complete(code, valid);
        ok &= check(f.client.state() == AetherPDTestClient::State::Failed && f.network.sent.size() == 1, "refusal/redirect has no retry");
    }
    {
        Fixture f; f.client.begin(15000); f.network.last->complete(200, QByteArray(513, 'x'));
        ok &= check(f.client.state() == AetherPDTestClient::State::Failed, "oversize response refused");
    }
    {
        Fixture f; f.client.begin(15000); f.now += 7500; f.client.poll();
        ok &= check(f.network.last->aborted && f.client.state() == AetherPDTestClient::State::Failed, "begin deadline enforced");
    }
    {
        Network network;
        qint64 now = 1000;
        std::unique_ptr<AetherPDTestClient> client = std::make_unique<AetherPDTestClient>(network, [&] { return now; });
        client->setSession(QHostAddress(QStringLiteral("192.168.50.100")), kToken, [] { return true; });
        client->setChangedCallback([&] { client.reset(); });
        client->begin(15000); network.last->complete(200, observation(15000));
        network.last->complete(200, observation(14999));
        ok &= check(!client, "callback may destroy client");
    }
    {
        std::unique_ptr<Network> network = std::make_unique<Network>();
        AetherPDTestClient client(*network, [] { return 1000; });
        client.setSession(QHostAddress(QStringLiteral("192.168.50.100")), kToken, [] { return true; });
        client.begin(15000); network.reset();
        ok &= check(client.state() == AetherPDTestClient::State::Failed, "transport destruction retires test");
    }
    return ok ? 0 : 1;
}
