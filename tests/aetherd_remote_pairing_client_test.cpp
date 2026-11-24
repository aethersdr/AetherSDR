// The desktop's half of pairing with a remote aetherd
// (docs/aetherd-remote-backend.md §3). Against an in-process engine over
// wss:// on 127.0.0.1, a code whose check
// digits do not match the certificate sends nothing, the right code pairs, and
// the stored record connects the backend.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/aetherd/RemoteEngineBackend.h"
#include "core/backends/aetherd/RemoteEnginePairing.h"
#include "core/backends/aetherd/RemoteEngineStore.h"
#include "core/control/ControlEngine.h"
#include "core/control/ControlPairing.h"
#include "core/control/LocalControlServer.h"
#include "core/control/RemoteControlServer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <cstdio>
#include <functional>

using namespace AetherSDR;
using namespace AetherSDR::aetherd;
using namespace AetherSDR::control;

namespace {
int failures = 0;
void check(bool condition, const QString& message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", qPrintable(message));
    failures += !condition;
}
bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return predicate();
}
class Issuer final : public PairingIssuer {
public:
    explicit Issuer(ControlCredentials& credentials) : m_credentials(credentials) {}
    QStringList names;
    void issue(const QString& name, Done done) override
    {
        names.append(name);
        QByteArray secret;
        auto record = ControlCredentials::generateRemote(ControlCredentials::Role::RemoteControl, &secret);
        record.name = name;
        QTimer::singleShot(10, [this, record, secret, done = std::move(done)] {
            done(m_credentials.add(record)
                     ? std::optional{Issued{record.id, QString::fromLatin1(secret.toHex())}}
                     : std::nullopt);
        });
    }
private:
    ControlCredentials& m_credentials;
};

QJsonObject localExchange(QLocalSocket& socket, const QJsonObject& request)
{
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    QJsonObject answer;
    waitUntil([&] {
        if (!socket.canReadLine()) { return false; }
        answer = QJsonDocument::fromJson(socket.readLine()).object();
        return true;
    });
    return answer;
}
} // namespace

int main(int argc, char* argv[])
{
    TestSettingsProfile profile(QStringLiteral("aetherd-remote-pairing-client"));
    if (!profile.isValid()) { return 1; }
    QTemporaryDir runtime;
    qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
    QFile::setPermissions(runtime.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    const QString openssl = QStandardPaths::findExecutable("openssl");
    if (openssl.isEmpty()) { std::printf("SKIP: no openssl tool\n"); return failures ? 1 : 77; }
    QTemporaryDir tls;
    const QString cert = tls.filePath("cert.pem");
    const QString key = tls.filePath("key.pem");
    QProcess::execute(openssl, {"req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
        "-nodes", "-days", "1", "-subj", "/CN=aetherd-test", "-keyout", key, "-out", cert});
    QString why;
    const auto identity = RemoteControlServer::loadTlsIdentity(cert, key, &why);
    if (!identity) { std::printf("FAIL: no certificate (%s)\n", qPrintable(why)); return 1; }
    const QString fingerprint = RemoteControlServer::certificateSha256(*identity);

    ControlEngine engine;
    ControlCredentials credentials;
    Issuer issuer(credentials);
    (void)engine.resources.upsert({"radioSession", {}, "radio-1"},
                                  {{"id", "radio-1"}, {"connected", true}, {"family", "anan"},
                                   {"identity", QJsonObject{{"model", "ANAN-G2"}}}});
    LocalControlServer local(engine, nullptr, {}, true);
    RemoteControlServer remote(engine, RemoteControlServer::Mode::Secure, nullptr);
    if (!credentials.replace({}) || !local.bindCredentials(&credentials)
        || !engine.service.bindPairing({&issuer, fingerprint, [] { return qint64(0); }})
        || !remote.setTlsConfiguration(*identity)) {
        std::printf("FAIL: cannot compose the engine\n");
        return 1;
    }
    if (!local.listen("pairing-client-" + QUuid::createUuid().toString(QUuid::Id128),
                      LocalControlServer::ListenMode::ReserveEndpoint)
        || !remote.listen({QHostAddress::LocalHost, 0}, &why)) {
        std::printf("SKIP: cannot bind (%s)\n", qPrintable(why));
        return failures ? 1 : 77;
    }
    if (!local.startServing() || !remote.startServing()) { std::printf("FAIL: not serving\n"); return 1; }
    const QString url = QStringLiteral("wss://127.0.0.1:%1").arg(remote.serverPort());

    QLocalSocket panel;
    panel.connectToServer(local.fullServerName());
    check(panel.waitForConnected(2000), "the radio's own panel connects locally");
    const QString sessionId = localExchange(panel, {{"v", 1}, {"id", "h"}, {"method", "hello"},
        {"params", QJsonObject{{"versions", QJsonArray{1}}}}}).value("result").toObject().value("sessionId").toString();
    const QString code = localExchange(panel, {{"v", 1}, {"id", "p"}, {"method", "pairing.begin"},
        {"params", QJsonObject{}}, {"sessionId", sessionId}}).value("result").toObject().value("code").toString();
    const std::optional<QString> digits = ControlPairing::digitsOf(code);
    check(digits && digits->size() == 10, "the radio's screen shows a ten-digit code");
    if (!digits) { return 1; }

    // A man in the middle shows up as check digits that do not match.
    {
        QString tampered = *digits;
        tampered[9] = QChar(u'0' + ((tampered[9].unicode() - u'0' + 1) % 10));
        RemoteEnginePairing pairing;
        QString failure;
        QObject::connect(&pairing, &RemoteEnginePairing::failed, [&](const QString& reason) { failure = reason; });
        pairing.start(url, tampered, "desktop");
        check(waitUntil([&] { return !failure.isEmpty(); }), "a code that does not match the certificate fails");
        check(failure.contains("does not match") && issuer.names.isEmpty(),
              "and nothing was sent: no credential was issued");
    }
    // The right code pairs, and the record connects the backend.
    RemoteEngineRecord paired;
    {
        RemoteEnginePairing pairing;
        QString failure;
        QObject::connect(&pairing, &RemoteEnginePairing::failed, [&](const QString& reason) { failure = reason; });
        QObject::connect(&pairing, &RemoteEnginePairing::paired, [&](const RemoteEngineRecord& r) { paired = r; });
        pairing.start(url, code, QStringLiteral("  a-desktop-with-a-rather-long-host-name.example  "), "engine");
        check(waitUntil([&] { return !paired.token.isEmpty() || !failure.isEmpty(); }),
              QStringLiteral("the right code pairs (%1)").arg(failure));
        check(paired.engineId == fingerprint && paired.token.size() == 64 && paired.lanUrl == url,
              "the record names the certificate it paired against and carries the token");
        check(issuer.names.size() == 1 && issuer.names.first().toUtf8().size() <= 32,
              "the client name is trimmed to 32 bytes for the radio's list");
    }
    RemoteEngineStore::save(paired);
    {
        RemoteEngineBackend backend;
        bool connected = false;
        QObject::connect(&backend, &IRadioBackend::connected, [&] { connected = true; });
        RadioConnectRequest request;
        request.host = QStringLiteral("127.0.0.1");
        request.port = remote.serverPort();
        request.serial = paired.engineId;
        backend.connectRadio(request);
        check(waitUntil([&] { return connected; }), "the paired token connects the remote-engine backend");
        backend.disconnectRadio();
    }
    RemoteEngineStore::forget(paired.engineId);
    RemoteEngineCredentials::clearAll();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
