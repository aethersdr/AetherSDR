// Explicit opt-in integration diagnostic for OUR daemon and built-in Demo.
// Never a stand-in for third-party firmware and never a TX/hardware test.
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <cstdio>
#include <functional>

namespace {
class Client {
public:
    QLocalSocket socket;
    QString session;
    QString failure;
    qint64 sequence{0};
    QHash<QString, QJsonObject> latest;
    int nextId{0};

    bool attach(const QString& endpoint)
    {
        socket.connectToServer(endpoint);
        return socket.waitForConnected(500);
    }
    QJsonObject call(const QString& method, const QJsonObject& params = {})
    {
        const QString id = QString::number(++nextId);
        QJsonObject request{{"v", 1}, {"id", id}, {"method", method}, {"params", params}};
        if (!session.isEmpty()) { request.insert("sessionId", session); }
        socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
        socket.flush();
        QElapsedTimer clock; clock.start();
        while (clock.elapsed() < 5000) {
            if (!socket.canReadLine()) { socket.waitForReadyRead(100); }
            while (socket.canReadLine()) {
                const QByteArray line = socket.readLine(256 * 1024 + 1);
                if (!line.endsWith('\n')) { failure = "oversized/incomplete daemon frame"; return {}; }
                const QJsonObject message = QJsonDocument::fromJson(line).object();
                if (message.contains("event")) {
                    observe(message);
                } else if (message.value("id") == id) {
                    if (message.contains("error")) {
                        failure = QJsonDocument(message.value("error").toObject()).toJson(QJsonDocument::Compact);
                        return {};
                    }
                    return message.value("result").toObject();
                }
            }
            if (socket.state() == QLocalSocket::UnconnectedState) {
                failure = "daemon disconnected before responding";
                return {};
            }
        }
        failure = "daemon response timeout";
        return {};
    }
    void observe(const QJsonObject& message)
    {
        const qint64 next = message.value("sequence").toInteger();
        if (next <= sequence) { failure = "non-monotonic event sequence"; }
        sequence = next;
        const QJsonObject resource = message.value("resource").toObject();
        const QString key = resource.value("type").toString() + '/' + resource.value("id").toString();
        if (latest.size() < 128 || latest.contains(key)) { latest.insert(key, message); }
    }
    QJsonObject get(const QString& type, const QString& id = {})
    {
        QJsonObject resource{{"type", type}};
        if (type != "radioCatalogue" && type != "server" && type != "radioSession") {
            resource.insert("radioSession", "radio-1");
        }
        if (!id.isEmpty()) { resource.insert("id", id); }
        return call("resource.get", {{"resource", resource}});
    }
};

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QElapsedTimer clock; clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (predicate()) { return true; }
        QThread::msleep(100);
    }
    return false;
}

bool check(bool ok, const char* description)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", description);
    return ok;
}

bool exercise(Client& controller, Client& observer)
{
    observer.session = observer.call("hello", {{"versions", QJsonArray{1}}}).value("sessionId").toString();
    if (!check(!controller.session.isEmpty() && !observer.session.isEmpty() && controller.session != observer.session,
               "two local clients negotiated independent sessions")) { return false; }
    const QJsonObject baseline = observer.call("resource.subscribe", {{"resources", QJsonArray{
        QJsonObject{{"type", "slice"}, {"radioSession", "radio-1"}},
        QJsonObject{{"type", "meter"}, {"radioSession", "radio-1"}},
        QJsonObject{{"type", "transmitState"}, {"radioSession", "radio-1"}}}}});
    if (!check(baseline.contains("subscription"), "observer installed atomic resource subscription")) { return false; }
    for (int cycle = 0; cycle < 2; ++cycle) {
        const QJsonObject catalogue = controller.get("radioCatalogue");
        const QJsonArray entries = catalogue.value("value").toObject().value("entries").toArray();
        QJsonObject demo;
        for (const QJsonValue& value : entries) {
            const auto entry = value.toObject();
            if (entry.value("family") == "sim" && entry.value("serial") == "DEMO-0001") { demo = entry; }
        }
        if (!check(!demo.isEmpty(), "catalogue offers exact Demo identity without LAN/USB discovery")) { return false; }
        const auto connected = controller.call("radio.connect", {{"radioSession", "radio-1"},
            {"radioId", demo.value("id")}, {"catalogueRevision", catalogue.value("revision")}});
        if (!check(connected.value("accepted").toBool(), "explicit Demo connection intent accepted")) { return false; }
        if (!check(waitUntil([&] {
                const auto caps = controller.call("capabilities.get").value("capabilities").toArray();
                return caps.contains("slice.setFrequency") && caps.contains("slice.setMode");
            }), "Demo publishes qualified frequency and mode controls")) { return false; }
        for (const QString& mode : {QStringLiteral("LSB"), QStringLiteral("USB")}) {
            const auto slice = controller.get("slice", "0");
            const auto response = controller.call("slice.setMode", {{"radioSession", "radio-1"}, {"slice", "0"},
                {"expectedRevision", slice.value("revision")}, {"mode", mode}});
            if (!check(response.value("accepted").toBool(), "typed mode intent accepted")) { return false; }
            if (!check(waitUntil([&] {
                const auto value = observer.get("slice", "0").value("value").toObject();
                return value.value("receiveObservation").toObject().value("mode").toObject().value("value") == mode;
            }), "second client converges to authoritative mode readback")) { return false; }
        }
        const auto slice = controller.get("slice", "0");
        const int hz = 14230000 + cycle * 1000;
        if (!check(controller.call("slice.setFrequency", {{"radioSession", "radio-1"}, {"slice", "0"},
                {"expectedRevision", slice.value("revision")}, {"hz", hz}}).value("accepted").toBool(),
                "existing frequency intent remains usable")) { return false; }
        if (!check(waitUntil([&] {
                const auto value = observer.get("slice", "0").value("value").toObject();
                return value.value("frequencyObservation").toObject().value("hz").toInt() == hz;
            }), "second client converges to authoritative frequency readback")) { return false; }
        // Normal Demo RX produces audio/spectrum, not MeterModel samples.
        // Sample delivery/freshness is covered by injected-transport tests;
        // do not add synthetic telemetry just to make this diagnostic pass.
        const auto delivery = observer.get("radioSession", "radio-1").value("value").toObject()
            .value("meterDelivery").toObject();
        if (!check(delivery.value("maxEntries").toInt() == 64
                && delivery.value("publishIntervalMs").toInt() == 100
                && delivery.value("staleAfterMs").toInt() == 2000,
                "second client reads the bounded meter delivery contract")) { return false; }
        if (!check(observer.get("transmitState").value("value").toObject().value("state") == "unsupported",
                   "read-only transmit state correctly describes RX-only Demo")) { return false; }
        if (!check(controller.call("radio.disconnect", {{"radioSession", "radio-1"}}).value("accepted").toBool(),
                   "explicit shared-session disconnect accepted")) { return false; }
        if (!check(waitUntil([&] {
                return controller.get("radioSession", "radio-1").value("value").toObject()
                    .value("connectionControl").toObject().value("state") == "idle";
            }), "daemon returns to idle before reconnect")) { return false; }
        observer.call("capabilities.get");
        observer.latest.clear();
    }
    return check(controller.failure.isEmpty() && observer.failure.isEmpty(), "responses and event sequences remain valid across reconnect");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("aetherd"));
    if (app.arguments().size() != 2 || !QFileInfo::exists(app.arguments().at(1))) {
        std::fprintf(stderr, "usage: aetherd_receive_smoke /absolute/path/to/aetherd[.exe]\n");
        return 2;
    }
    QTemporaryDir scratch;
    if (!scratch.isValid()) { return 1; }
    const QString logicalEndpoint = "aetherd-receive-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    // Client-side locator for LocalControlServer's current-user transport.
    // The daemon alone creates/checks the private runtime directory and lock.
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(
        logicalEndpoint.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
#ifdef Q_OS_WIN
    const QString endpoint = QStringLiteral("aethersdr-%1").arg(digest);
#else
    const QString endpoint = QDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation))
        .filePath(QStringLiteral("aethersdr/control-%1.sock").arg(digest));
#endif
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove("AETHER_AUTOMATION_ALLOW_TX");
    env.insert("AETHER_AUTOMATION", "1");
    env.insert("AETHER_AUTOMATION_NO_TX", "1");
    env.insert("AETHER_SETTINGS_DIR", scratch.path());
    QProcess daemon;
    daemon.setProcessEnvironment(env);
    daemon.setProcessChannelMode(QProcess::MergedChannels);
    daemon.start(QFileInfo(app.arguments().at(1)).absoluteFilePath(),
        {"--socket", logicalEndpoint, "--discover-sim", "--allow-local-control"});
    if (!daemon.waitForStarted(3000)) { return 1; }
    Client controller;
    // Readiness is a successful hello, not merely a connected pipe. The
    // daemon reserves its endpoint before constructors that may pump events;
    // an early connection is deliberately closed without creating a session.
    const bool attached = waitUntil([&] {
        controller.socket.abort();
        controller.failure.clear();
        if (controller.attach(endpoint)) {
            controller.session = controller.call("hello", {{"versions", QJsonArray{1}}})
                .value("sessionId").toString();
        }
        return !controller.session.isEmpty() || daemon.state() == QProcess::NotRunning;
    }, 15000) && !controller.session.isEmpty();
    Client observer;
    const bool ok = attached && observer.attach(endpoint) && exercise(controller, observer);
    controller.socket.abort(); observer.socket.abort();
    daemon.terminate();
    if (!daemon.waitForFinished(3000)) { daemon.kill(); daemon.waitForFinished(3000); }
    const QByteArray log = daemon.readAll();
    if (!ok) {
        std::fprintf(stderr, "%s\n%s\n%s\n", qPrintable(controller.failure), qPrintable(observer.failure), log.right(4096).constData());
        if (log.contains("cannot listen")) { return 77; }
    }
    return ok ? 0 : 1;
}
