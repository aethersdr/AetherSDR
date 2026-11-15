// Our shim API, with injected HTTP and time. No socket or firmware simulator.
#include "AetherPDTestNetwork.h"
#include "core/AetherPDInstallRequestClient.h"

#include <QCryptographicHash>
#include <QEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QtEndian>

#include <cstddef>
#include <memory>
#include <optional>

using Client = AetherSDR::AetherPDInstallRequestClient;
using State = Client::State;
using Reason = Client::Reason;
namespace {
const QHostAddress address(QStringLiteral("192.168.50.100"));
const QString token(43, u'a');
QByteArray plan()
{
    QFile file(QStringLiteral(AETHERPD_INSTALLATION_TESTDATA_DIR "/approved.hex"));
    if (!file.open(QIODevice::ReadOnly)) { return {}; }
    return QByteArray::fromHex(file.readAll().trimmed());
}
QByteArray config(const QByteArray& approved)
{
    QByteArray receipt("PDDELIV1");
    receipt.resize(16);
    qToBigEndian<quint32>(64, receipt.data() + 8);
    qToBigEndian<quint32>(2, receipt.data() + 12);
    receipt += QByteArray(32, 0x33) + QCryptographicHash::hash(approved, QCryptographicHash::Blake2b_256);
    return QByteArray("PDXFER01") + QByteArray(32, 0x5a) + receipt;
}
QByteArray encode(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject ready(const Client& client)
{
    return {{"state", "awaiting_inputs"}, {"request_id", client.requestId()}, {"port", 48995}, {"installation_committed", false}};
}
QJsonObject running(const Client& client)
{
    return {{"state", "running"}, {"request_id", client.requestId()}, {"installation_committed", false}};
}
bool publicChecks()
{
    bool ok = true;
    const QByteArray approved = plan(), privateBytes = config(approved);
    const std::optional<QByteArray> publicBytes = Client::publicRecord(privateBytes, approved);
    ok &= check(publicBytes && publicBytes->size() == 120 && publicBytes->startsWith("PDPUB001")
        && publicBytes->mid(8, 80) == privateBytes.mid(40)
        && publicBytes->last(32).toHex() == "d6893758984fd2a4aac121dd6107ec80657d04dd20c95928ffb55e86b8d70ea1",
        "public record matches Python BLAKE2b vector over native approved fixture");
    for (qsizetype size = 0; size < privateBytes.size(); ++size) {
        ok &= check(!Client::publicRecord(privateBytes.first(size), approved), "short private request refused");
    }
    ok &= check(!Client::publicRecord(privateBytes + 'x', approved), "long private request refused");
    for (const int offset : {0, 40, 48, 52, 88}) {
        QByteArray bad = privateBytes; bad[offset] ^= 1;
        ok &= check(!Client::publicRecord(bad, approved), "wrong magic architecture role or plan refused");
    }
    for (const int offset : {8, 56}) {
        QByteArray bad = privateBytes; bad.replace(offset, 32, QByteArray(32, '\0'));
        ok &= check(!Client::publicRecord(bad, approved), "zero secret or worker digest refused");
    }
    QByteArray armPlan = approved;
    qToBigEndian<quint32>(2, armPlan.data() + 16);
    qToBigEndian<quint32>(2, armPlan.data() + 156);
    QByteArray armConfig = config(armPlan);
    ok &= check(!Client::publicRecord(armConfig, armPlan), "cross-profile request refused");
    qToBigEndian<quint32>(7, armConfig.data() + 48);
    ok &= check(Client::publicRecord(armConfig, armPlan).has_value(), "ARMv7 record supported without claiming hardware qualification");
    return ok;
}
bool lifecycle()
{
    bool ok = true;
    qint64 time = 0;
    Network network;
    Client client(network, [&] { return time; });
    int observed = 0, failures = 0, result = -1;
    QObject::connect(&client, &Client::observed, [&](State, int value) { ++observed; result = value; });
    QObject::connect(&client, &Client::failed, [&](Reason) { ++failures; });
    ok &= check(!client.inspect() && !client.requestStop(), "idle cannot issue control");
    ok &= check(!client.begin(QHostAddress("8.8.8.8"), token, config(plan()), plan())
        && !client.begin(address, "bad\r\n", config(plan()), plan()) && network.sent.empty(), "bad target/token causes no request");
    ok &= check(client.begin(address, token, config(plan()), plan()), "begin admitted");
    ok &= check(network.sent.size() == 1 && network.sent[0].method == QNetworkAccessManager::PostOperation
        && network.sent[0].body == config(plan())
        && network.sent[0].request.url().path() == "/v1/aetherpd/install-request"
        && network.sent[0].request.url().port() == 48992
        && network.sent[0].request.rawHeader("Authorization") == "Bearer " + token.toLatin1()
        && network.sent[0].request.header(QNetworkRequest::ContentTypeHeader) == "application/octet-stream"
        && network.sent[0].request.attribute(QNetworkRequest::RedirectPolicyAttribute) == QNetworkRequest::ManualRedirectPolicy
        && network.sent[0].request.attribute(QNetworkRequest::CacheLoadControlAttribute) == QNetworkRequest::AlwaysNetwork
        && !network.sent[0].request.attribute(QNetworkRequest::CacheSaveControlAttribute).toBool(), "fixed authenticated uncached mutation");
    ok &= check(!client.inspect() && !client.requestStop() && !client.begin(address, token, config(plan()), plan()), "no overlapping or repeated begin");
    network.last->complete(202, encode(ready(client)));
    ok &= check(client.state() == State::AwaitingInputs && observed == 1 && result == -1, "readiness only");
    ok &= check(client.inspect(), "explicit inspection allowed");
    ok &= check(network.sent.back().method == QNetworkAccessManager::GetOperation && network.sent.back().body.isEmpty()
        && network.sent.back().request.url().path().endsWith('/' + client.requestId()), "inspection bound to own public commitment");
    network.last->complete(200, encode(running(client)));
    ok &= check(client.state() == State::Running, "running observed");
    ok &= check(client.requestStop(), "explicit stop allowed");
    ok &= check(network.sent.back().method == QNetworkAccessManager::DeleteOperation && network.sent.back().body.isEmpty(), "empty fixed stop");
    network.last->complete(202, "{\"state\":\"stop_requested\"}");
    ok &= check(client.state() == State::StopRequested, "stop acknowledgement does not assert remote cleanup");
    for (const int code : {0, 3, 4, 5, 6}) {
        ok &= check(client.inspect(), "inspect delivery");
        QJsonObject terminal = running(client); terminal["state"] = "delivery_finished"; terminal["delivery_result"] = code;
        network.last->complete(200, encode(terminal));
        ok &= check(client.state() == State::DeliveryFinished && result == code, "delivery result retained without commit claim");
    }
    ok &= check(client.inspect(), "inspect after restart");
    network.last->complete(404, "{\"error\":\"unknown installer request\"}", QNetworkReply::ContentNotFoundError);
    ok &= check(client.state() == State::Missing && result == -1 && failures == 0, "missing request is not failed installation");
    time = 400000; client.poll();
    ok &= check(client.state() == State::Expired && failures == 1 && !client.inspect() && !client.requestStop(), "credential lifetime bounded");
    return ok;
}
bool failures()
{
    bool ok = true;
    for (const int scenario : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}) {
        qint64 time = 0;
        Network network;
        Client client(network, [&] { return time; });
        int observations = 0, errors = 0;
        QObject::connect(&client, &Client::observed, [&](State, int) { ++observations; });
        QObject::connect(&client, &Client::failed, [&](Reason) { ++errors; });
        ok &= check(client.begin(address, token, config(plan()), plan()), "failure case begin");
        QJsonObject response = ready(client);
        int status = 202;
        if (scenario == 0) { response["request_id"] = QString(64, u'1'); }
        if (scenario == 1) { response["port"] = 4992; }
        if (scenario == 2) { response["installation_committed"] = true; }
        if (scenario == 3) { response.remove("request_id"); }
        if (scenario == 4) { response["port"] = "48995"; }
        if (scenario == 5) { status = 302; }
        if (scenario == 6) { status = 401; }
        if (scenario == 7) { status = 409; }
        if (scenario == 8) { response["command"] = "ignored?"; }
        if (scenario == 9) { time = 10000; client.poll(); }
        else if (scenario == 10) { network.last->complete(202, QByteArray(2049, 'x')); }
        else if (scenario == 11) { network.last->complete(202, encode(response), QNetworkReply::RemoteHostClosedError); }
        else { network.last->complete(status, encode(response)); }
        ok &= check(errors == 1 && observations == 0, "untrusted or lost acknowledgement never admitted");
        const std::size_t before = network.sent.size();
        time += 10001; client.poll();
        ok &= check(network.sent.size() == before && !client.begin(address, token, config(plan()), plan()), "mutation never automatically or explicitly replayed");
        if (scenario != 6) {
            ok &= check(client.inspect(), "ambiguous begin permits reconciliation of same known ID");
            network.last->complete(200, encode(running(client)));
            ok &= check(client.state() == State::Running, "reconciled same request after lost reply");
        } else { ok &= check(!client.inspect(), "unauthorized retires context"); }
    }
    // GET/DELETE responses must not turn a malformed result into delivery success.
    for (const double result : {-1., 1., 2., 3.5, 7.}) {
        qint64 time = 0; Network network; Client client(network, [&] { return time; });
        client.begin(address, token, config(plan()), plan()); network.last->complete(202, encode(ready(client)));
        client.inspect(); QJsonObject response = running(client); response["state"] = "delivery_finished"; response["delivery_result"] = result;
        network.last->complete(200, encode(response));
        ok &= check(client.state() == State::Unknown, "unknown or fractional delivery result refused");
    }
    return ok;
}
bool lifetime()
{
    bool ok = true;
    qint64 time = 0;
    Network network;
    auto client = std::make_unique<Client>(network, [&] { return time; });
    client->begin(address, token, config(plan()), plan());
    QPointer<Reply> pending = network.last;
    client.reset();
    ok &= check(pending && pending->aborted && network.sent.size() == 1, "destruction aborts local request without extra mutation");
    auto transport = std::make_unique<Network>();
    Client owned(*transport, [&] { return time; });
    owned.begin(address, token, config(plan()), plan());
    transport.reset();
    ok &= check(owned.state() == State::Expired && !owned.requestStop(), "manager death retires credential context");
    client = std::make_unique<Client>(network, [&] { return time; });
    client->begin(address, token, config(plan()), plan());
    QObject::connect(client.get(), &Client::observed, [&] { client.reset(); });
    const QByteArray reply = encode(ready(*client));
    network.last->complete(202, reply);
    ok &= check(!client, "observer can destroy client in completion");
    client = std::make_unique<Client>(network, [&] { return time; });
    client->begin(address, token, config(plan()), plan());
    QObject::connect(network.last, &QNetworkReply::finished, [&] { client.reset(); });
    time = 10000; client->poll();
    ok &= check(!client, "abort observer can destroy client without use after free");
    Client badClock(network, [&] { return time; });
    badClock.begin(address, token, config(plan()), plan());
    --time; badClock.poll();
    ok &= check(badClock.state() == State::Expired, "backwards clock retires context");
    return ok;
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const bool ok = publicChecks() & lifecycle() & failures() & lifetime();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return ok ? 0 : 1;
}
