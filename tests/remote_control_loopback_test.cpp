// The network listener over a real loopback wss:// connection
// (aetherd-remote-clients.md §4): a client pins the certificate it sees
// against the pairing code's check digits, pairs, reconnects with its token,
// and a change it makes is mirrored to a local-socket client of the same
// engine. Also: the session budget, binary frames and embedded line breaks.
// Binds 127.0.0.1 on an ephemeral port and a local socket in a private
// runtime directory. Needs the openssl tool for a throwaway certificate; exits
// 77 (skipped) without it.
#include "core/control/ControlEngine.h"
#include "core/control/LocalControlServer.h"
#include "core/control/RemoteControlServer.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QTimer>
#include <QUrl>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QSslCertificate>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QWebSocket>
#include <cstdio>
#include <functional>

using namespace AetherSDR::control;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}
bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}
QString error(const QJsonObject& reply) { return reply.value("error").toObject().value("code").toString(); }
QJsonObject result(const QJsonObject& reply) { return reply.value("result").toObject(); }
QJsonObject request(const QString& method, QJsonObject params, const QString& sessionId = {})
{
    QJsonObject r{{"v", 1}, {"id", method}, {"method", method}, {"params", params}};
    if (!sessionId.isEmpty()) { r.insert("sessionId", sessionId); }
    return r;
}
QJsonObject hello(const QString& token = {})
{
    QJsonObject p{{"versions", QJsonArray{1}}};
    if (!token.isEmpty()) { p.insert("auth", QJsonObject{{"scheme", "bearer"}, {"token", token}}); }
    return request("hello", p);
}

// The radio's speaker as the only receive control; applying it publishes the
// new state the way the model adapter does, so every subscriber sees it.
class SpeakerTarget final : public ReceiveControlTarget {
public:
    explicit SpeakerTarget(ControlResourceStore& store) : m_store(store) {}
    bool available(ReceiveOperation operation) const override { return operation == ReceiveOperation::RadioSpeaker; }
    std::optional<ProtocolError> setRadioSpeaker(bool enabled) override
    {
        (void)m_store.upsert({"radioSession", {}, "radio-1"}, {{"radioSpeaker", enabled}});
        return std::nullopt;
    }
    std::optional<ProtocolError> setMode(int, const QString&) override { return unsupported(); }
    std::optional<ProtocolError> setFilter(int, int, int) override { return unsupported(); }
    std::optional<ProtocolError> setAudioGain(int, int) override { return unsupported(); }
    std::optional<ProtocolError> setAudioMute(int, bool) override { return unsupported(); }
    std::optional<ProtocolError> setPanCenter(const QString&, qint64) override { return unsupported(); }
    std::optional<ProtocolError> setPanBandwidth(const QString&, qint64) override { return unsupported(); }
    std::optional<ProtocolError> setCwPitch(int) override { return unsupported(); }
    std::optional<ProtocolError> setAgcThreshold(int, int) override { return unsupported(); }
    std::optional<ProtocolError> setNoiseBlanker(int, bool) override { return unsupported(); }
    std::optional<ProtocolError> setNoiseReduction(int, bool) override { return unsupported(); }
    std::optional<ProtocolError> setRit(int, bool, int) override { return unsupported(); }
    std::optional<ProtocolError> setPanRfGain(const QString&, int) override { return unsupported(); }
private:
    static ProtocolError unsupported() { return {"capability.unavailable", "test target", {}, false}; }
    ControlResourceStore& m_store;
};

class Issuer final : public PairingIssuer {
public:
    explicit Issuer(ControlCredentials& credentials) : m_credentials(credentials) {}
    int delayMs{20};
    void issue(const QString& name, Done done) override
    {
        QByteArray secret;
        auto record = ControlCredentials::generateRemote(ControlCredentials::Role::RemoteControl, &secret);
        record.name = name;
        // Complete later, as the vault does.
        QTimer::singleShot(delayMs, [this, record, secret, done = std::move(done)] {
            done(m_credentials.add(record)
                     ? std::optional{Issued{record.id, QString::fromLatin1(secret.toHex())}}
                     : std::nullopt);
        });
    }
private:
    ControlCredentials& m_credentials;
};

// A wss:// client that trusts only the certificate it was told to pin.
struct WsClient {
    QWebSocket socket;
    QList<QJsonObject> received;
    QString seenFingerprint;
    bool closed{false};
    explicit WsClient(const QString& pin = {})
    {
        QObject::connect(&socket, &QWebSocket::textMessageReceived, [this](const QString& text) {
            received.append(QJsonDocument::fromJson(text.toUtf8()).object());
        });
        QObject::connect(&socket, &QWebSocket::disconnected, [this] { closed = true; });
        QObject::connect(&socket, &QWebSocket::sslErrors, [this, pin](const QList<QSslError>& errors) {
            const QSslCertificate peer = socket.sslConfiguration().peerCertificate();
            seenFingerprint = QString::fromLatin1(peer.digest(QCryptographicHash::Sha256).toHex());
            if (!pin.isEmpty() && seenFingerprint == pin) { socket.ignoreSslErrors(errors); }
        });
    }
    bool open(quint16 port, const char* scheme = "wss")
    {
        socket.open(QUrl(QStringLiteral("%1://127.0.0.1:%2/").arg(QLatin1String(scheme)).arg(port)));
        return waitUntil([this] { return socket.state() == QAbstractSocket::ConnectedState || closed; })
            && socket.state() == QAbstractSocket::ConnectedState;
    }
    QJsonObject exchange(const QJsonObject& r)
    {
        const qsizetype before = received.size();
        socket.sendTextMessage(QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
        return waitUntil([&] { return received.size() > before; }) ? received.at(before) : QJsonObject{};
    }
};

struct LocalClient {
    QLocalSocket socket;
    QList<QJsonObject> received;
    LocalClient()
    {
        QObject::connect(&socket, &QLocalSocket::readyRead, [this] {
            while (socket.canReadLine()) {
                received.append(QJsonDocument::fromJson(socket.readLine()).object());
            }
        });
    }
    QJsonObject exchange(const QJsonObject& r)
    {
        const qsizetype before = received.size();
        socket.write(QJsonDocument(r).toJson(QJsonDocument::Compact) + '\n');
        return waitUntil([&] { return received.size() > before; }) ? received.at(before) : QJsonObject{};
    }
};

bool sawSpeaker(const QList<QJsonObject>& messages, bool enabled)
{
    for (const QJsonObject& m : messages) {
        if (m.value("event") == QJsonValue("resource.changed")
            && m.value("resource").toObject().value("id") == QJsonValue("radio-1")
            && m.value("value").toObject().value("radioSpeaker") == QJsonValue(enabled)) {
            return true;
        }
    }
    return false;
}
} // namespace

int main(int argc, char* argv[])
{
    // Never the real runtime directory (the operator's own aetherd lives there).
    QTemporaryDir runtime;
    qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
    QFile::setPermissions(runtime.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QCoreApplication app(argc, argv);

    const QString openssl = QStandardPaths::findExecutable("openssl");
    if (openssl.isEmpty()) {
        std::printf("SKIP: no openssl tool for a throwaway certificate\n");
        return 77;
    }
    QTemporaryDir tls;
    const QString cert = tls.filePath("cert.pem");
    const QString key = tls.filePath("key.pem");
    const int made = QProcess::execute(openssl, {"req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
        "-nodes", "-days", "1", "-subj", "/CN=aetherd-test", "-keyout", key, "-out", cert});
    QString why;
    const auto identity = RemoteControlServer::loadTlsIdentity(cert, key, &why);
    check(made == 0 && identity.has_value(), "load a throwaway EC certificate and key");
    if (!identity) { return 1; }
    const QString fingerprint = RemoteControlServer::certificateSha256(*identity);

    ControlEngine engine;
    ControlCredentials credentials;
    SpeakerTarget speaker(engine.resources);
    Issuer issuer(credentials);
    (void)engine.resources.upsert({"radioSession", {}, "radio-1"}, {{"radioSpeaker", false}});
    LocalControlServer local(engine, nullptr, {}, true);
    RemoteControlServer remote(engine, RemoteControlServer::Mode::Secure, nullptr,
                               {2, RemoteControlServer::kHandshakeTimeoutMs, RemoteControlServer::kMaxQueuedOutputBytes});
    check(credentials.replace({}) && local.bindCredentials(&credentials) && local.bindReceiveTarget(&speaker)
              && engine.service.bindPairing({&issuer, fingerprint, [] { return qint64(0); }}),
          "compose one engine for both listeners");
    const bool localUp = local.listen("remote-loopback-" + QUuid::createUuid().toString(QUuid::Id128),
                                      LocalControlServer::ListenMode::ReserveEndpoint);
    const bool tlsSet = remote.setTlsConfiguration(*identity);
    const bool remoteUp = tlsSet && remote.listen({QHostAddress::LocalHost, 0}, &why);
    // tests-ci.md: a socket-owning test that cannot bind skips at once rather
    // than spending its timeout; a setup failure after binding fails at once.
    if (!tlsSet) { std::printf("FAIL: the TLS configuration was refused\n"); return 1; }
    if (!localUp || !remoteUp) {
        std::printf("SKIP: cannot bind the test listeners (%s)\n", qPrintable(why));
        return 77;
    }
    if (!local.startServing() || !remote.startServing()) {
        std::printf("FAIL: the listeners would not start serving\n");
        return 1;
    }
    check(true, "listen on the local socket and on 127.0.0.1");
    const quint16 port = remote.serverPort();

    LocalClient panel;
    panel.socket.connectToServer(local.fullServerName());
    check(panel.socket.waitForConnected(2000), "the local client connects");
    const QString panelSession = result(panel.exchange(hello())).value("sessionId").toString();
    const QJsonObject opened = result(panel.exchange(request("pairing.begin", {}, panelSession)));
    const QString code = opened.value("code").toString();
    check(code.size() == 12 && opened.value("certificateSha256").toString() == fingerprint,
          "the local client opens a pairing and is told the listener's fingerprint");

    // The client never saw the fingerprint: it derives the check digits from
    // the certificate the connection presents and compares them to the code.
    {
        WsClient probe;   // pins nothing: refuses the self-signed certificate
        check(!probe.open(port) && probe.seenFingerprint == fingerprint,
              "an unpinned client refuses the certificate and learns its fingerprint");
        const QString six = ControlPairing::digitsOf(code)->left(6);
        check(ControlPairing::checkDigits(six, probe.seenFingerprint) == code.right(4),
              "the code's check digits match the certificate the client actually saw");
        check(ControlPairing::checkDigits(six, QString(64, u'0')) != code.right(4),
              "another certificate would not match (1 in 10 000)");
    }
    QString token;
    {
        WsClient pairing(fingerprint);
        check(pairing.open(port), "a client that pinned the certificate completes TLS");
        const QJsonObject reply = pairing.exchange(request("pairing.complete", {{"code", code}, {"clientName", "Shack laptop"}}));
        token = result(reply).value("token").toString();
        check(token.size() == 64, "pairing.complete returns a token over the wire");
        check(waitUntil([&] { return pairing.closed; }), "the server closes the pairing connection");
    }

    WsClient laptop(fingerprint);
    check(laptop.open(port), "the paired client reconnects");
    const QJsonObject caps = result(laptop.exchange(hello(token)));
    const QString laptopSession = caps.value("sessionId").toString();
    check(caps.value("grants").toArray() == QJsonArray{"observe", "control"}
              && caps.value("capabilities").toArray().contains("radioSession.setRadioSpeaker"),
          "its token gives observe and control, and the speaker control is offered");
    const QJsonObject selector{{"type", "radioSession"}, {"id", "radio-1"}};
    const QJsonObject subscribed = laptop.exchange(request("resource.subscribe", {{"resources", QJsonArray{selector}}}, laptopSession));
    check(error(subscribed).isEmpty() && error(panel.exchange(request("resource.subscribe",
              {{"resources", QJsonArray{selector}}}, panelSession))).isEmpty(),
          "both clients subscribe to the radio session");
    const qint64 revision = engine.resources.get({"radioSession", {}, "radio-1"})->revision;
    const QJsonObject set = laptop.exchange(request("radioSession.setRadioSpeaker",
        {{"radioSession", "radio-1"}, {"expectedRevision", revision}, {"enabled", true}}, laptopSession));
    check(error(set).isEmpty(), "the network client changes the speaker");
    check(waitUntil([&] { return sawSpeaker(panel.received, true) && sawSpeaker(laptop.received, true); }),
          "the change is mirrored to the local client and to the network client");

    // Budget: two network sessions here; the pairing and probe ones are gone.
    {
        WsClient second(fingerprint);
        check(second.open(port) && waitUntil([&] { return remote.sessionCount() == 2; }), "a second session fits");
        WsClient third(fingerprint);
        third.open(port);
        check(waitUntil([&] { return third.closed; })
                  && !third.received.isEmpty() && error(third.received.first()) == "transport.limit_exceeded",
              "a session over the network budget is refused");
        // One budget for every network listener of the engine.
        RemoteControlServer other(engine, RemoteControlServer::Mode::Secure, nullptr,
                                  {2, RemoteControlServer::kHandshakeTimeoutMs, RemoteControlServer::kMaxQueuedOutputBytes,
                                   RemoteControlServer::kPairingWriteTimeoutMs});
        if (!other.setTlsConfiguration(*identity) || !other.listen({QHostAddress::LocalHost, 0}, &why)) {
            std::printf("SKIP: cannot bind the budget-sharing listener (%s)\n", qPrintable(why));
            return 77;
        }
        if (!other.startServing()) { std::printf("FAIL: the budget-sharing listener would not serve\n"); return 1; }
        WsClient elsewhere(fingerprint);
        elsewhere.open(other.serverPort());
        check(waitUntil([&] { return elsewhere.closed; }) && !elsewhere.received.isEmpty()
                  && error(elsewhere.received.first()) == "transport.limit_exceeded" && other.sessionCount() == 0,
              "the budget is shared: another listener of the same engine refuses too");
        check(error(panel.exchange(request("capabilities.get", {}, panelSession))).isEmpty(),
              "the local socket is unaffected by the network budget");
    }
    {
        WsClient binary(fingerprint);
        check(binary.open(port), "connect for a binary frame");
        binary.socket.sendBinaryMessage("{}");
        check(waitUntil([&] { return binary.closed; }), "a binary frame closes the connection");
        WsClient twoInOne(fingerprint);
        check(twoInOne.open(port), "connect for a smuggled second request");
        twoInOne.socket.sendTextMessage(QString::fromUtf8(QJsonDocument(hello(token)).toJson(QJsonDocument::Compact))
                                        + "\n" + QString::fromUtf8(QJsonDocument(hello(token)).toJson(QJsonDocument::Compact)));
        check(waitUntil([&] { return twoInOne.closed; })
                  && error(twoInOne.received.value(0)) == "transport.limit_exceeded",
              "a frame carrying a line break is refused, not split into two requests");
        WsClient plain;
        check(!plain.open(port, "ws"), "plain ws:// to the TLS port does not connect");
    }

    // A vault write slower than the handshake deadline still reaches its
    // client: the deadline is extended once while the reply is owed.
    {
        RemoteControlServer slow(engine, RemoteControlServer::Mode::Secure, nullptr,
                                 {4, 200, RemoteControlServer::kMaxQueuedOutputBytes, 3000});
        if (!slow.setTlsConfiguration(*identity)) { std::printf("FAIL: second TLS configuration refused\n"); return 1; }
        if (!slow.listen({QHostAddress::LocalHost, 0}, &why)) {
            std::printf("SKIP: cannot bind the second test listener (%s)\n", qPrintable(why));
            return 77;
        }
        if (!slow.startServing()) { std::printf("FAIL: the second listener would not serve\n"); return 1; }
        check(true, "a second TLS listener with a 200 ms handshake deadline");
        issuer.delayMs = 600;
        const QString slowCode = result(panel.exchange(request("pairing.begin", {}, panelSession))).value("code").toString();
        WsClient client(fingerprint);
        check(client.open(slow.serverPort()), "connect to the short-deadline listener");
        const qsizetype before = client.received.size();
        client.socket.sendTextMessage(QString::fromUtf8(QJsonDocument(request("pairing.complete",
            {{"code", slowCode}, {"clientName", "slow"}})).toJson(QJsonDocument::Compact)));
        check(waitUntil([&] { return client.received.size() > before; }, 3000)
                  && result(client.received.value(before)).value("token").toString().size() == 64,
              "a pairing whose write outlasts the handshake deadline still gets its token");
        WsClient idle(fingerprint);
        check(idle.open(slow.serverPort()) && waitUntil([&] { return idle.closed; }, 2000)
                  && error(idle.received.value(0)) == "engine.timeout",
              "a connection that sends nothing is still closed at the deadline");
        slow.close();
    }

    remote.close();
    check(waitUntil([&] { return laptop.closed; }), "closing the listener ends its sessions");
    local.close();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
