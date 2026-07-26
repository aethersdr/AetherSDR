#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QWebSocket>
#include <QWebSocketServer>

#include <cstdio>
#include <functional>

using namespace AetherSDR;

namespace
{

int failures = 0;

void check(bool condition, const char* description)
{
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        ++failures;
    }
}

bool spinUntil(const std::function<bool()>& predicate, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}

class BridgeClient
{
public:
    bool connectToServer(const QString& name)
    {
        m_socket.connectToServer(name);
        return spinUntil([this] {
            return m_socket.state() == QLocalSocket::ConnectedState;
        });
    }

    QJsonObject request(const QByteArray& line)
    {
        m_socket.write(line + '\n');
        m_socket.flush();
        if (!spinUntil([this] { return m_socket.canReadLine(); })) {
            return {};
        }
        return QJsonDocument::fromJson(m_socket.readLine()).object();
    }

private:
    QLocalSocket m_socket;
};

int traceIndex(const QJsonArray& entries, const QString& direction,
               const QString& text)
{
    for (int i = 0; i < entries.size(); ++i) {
        const QJsonObject entry = entries.at(i).toObject();
        if (entry.value(QStringLiteral("direction")).toString() == direction
            && entry.value(QStringLiteral("text")).toString() == text) {
            return i;
        }
    }
    return -1;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-tci-automation-test"));
    check(settingsProfile.isValid(), "isolated settings profile is available");

    QCoreApplication app(argc, argv);

    QWebSocketServer fakeTci(
        QStringLiteral("fake-tci"), QWebSocketServer::NonSecureMode);
    check(fakeTci.listen(QHostAddress::LocalHost, 0),
          "fake TCI server listens on loopback");

    AutomationServer automation;
    automation.setTciRouteSnapshotHandler([] {
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("contractVersion"), 1},
            {QStringLiteral("routeOwner"), QStringLiteral("external")},
            {QStringLiteral("rxSliceId"), 4},
            {QStringLiteral("txSliceId"), 7},
            {QStringLiteral("ownsRoute"), false},
            {QStringLiteral("splitRequested"), false},
            {QStringLiteral("endpoints"), QJsonArray{
                QJsonObject{{QStringLiteral("trx"), 0},
                            {QStringLiteral("sliceId"), 4},
                            {QStringLiteral("tx"), false}},
                QJsonObject{{QStringLiteral("trx"), 1},
                            {QStringLiteral("sliceId"), 7},
                            {QStringLiteral("tx"), true}},
            }},
        };
    });

    const QString bridgeName = QStringLiteral("aether-tci-automation-test-%1")
                                   .arg(QCoreApplication::applicationPid());
    check(automation.start(bridgeName), "automation bridge starts");

    BridgeClient bridge;
    check(bridge.connectToServer(bridgeName), "bridge client connects");

    QJsonObject response = bridge.request(QByteArrayLiteral("tci trace start"));
    check(response.value(QStringLiteral("ok")).toBool()
              && response.value(QStringLiteral("capturing")).toBool(),
          "tci trace start enables a fresh bounded transcript");

    response = bridge.request(
        QByteArray("tci start ") + QByteArray::number(fakeTci.serverPort()));
    check(response.value(QStringLiteral("ok")).toBool()
              && response.value(QStringLiteral("profile")).toString()
                  == QStringLiteral("wsjtx"),
          "tci start selects the WSJT-X simulator profile");

    check(spinUntil([&fakeTci] { return fakeTci.hasPendingConnections(); }),
          "simulator connects to the fake TCI server");
    QWebSocket* peer = fakeTci.nextPendingConnection();
    check(peer != nullptr, "fake TCI server accepts simulator connection");
    if (!peer) {
        automation.stop();
        return 1;
    }

    QStringList clientFrames;
    QObject::connect(peer, &QWebSocket::textMessageReceived,
                     [&clientFrames](const QString& text) {
        clientFrames.append(text);
    });

    peer->sendTextMessage(QStringLiteral(
        "protocol:ExpertSDR3,2.0;"
        "channels_count:2;"
        "vfo:0,0,14074000;"
        "vfo:0,1,14076000;"
        "split_enable:0,false;"
        "ready;"));
    check(spinUntil([&clientFrames] {
        return clientFrames.contains(QStringLiteral("audio_samplerate:48000;"))
            && clientFrames.contains(QStringLiteral("audio_start:0;"));
    }), "WSJT-X negotiation starts 48 kHz audio after ready");

    const QString wsjtxSequence = QStringLiteral(
        "split_enable:0,false;vfo:0,1,14076000;");
    response = bridge.request(
        QByteArrayLiteral("tci send ") + wsjtxSequence.toUtf8());
    check(response.value(QStringLiteral("ok")).toBool()
              && response.value(QStringLiteral("command")).toString()
                  == wsjtxSequence,
          "tci send preserves the ordered WSJT-X command frame");
    check(spinUntil([&clientFrames, &wsjtxSequence] {
        return clientFrames.contains(wsjtxSequence);
    }), "fake server receives split state before VFO B programming");

    peer->sendTextMessage(QStringLiteral(
        "split_enable:0,false;vfo:0,1,14076000;"));
    check(spinUntil([&bridge] {
        const QJsonObject status = bridge.request(
            QByteArrayLiteral("tci trace status 100"));
        return status.value(QStringLiteral("count")).toInt() >= 12;
    }), "trace records both client and server protocol directions");

    const QJsonObject trace = bridge.request(
        QByteArrayLiteral("tci trace status 100"));
    const QJsonArray entries = trace.value(QStringLiteral("entries")).toArray();
    const int readyIndex = traceIndex(
        entries, QStringLiteral("server->client"), QStringLiteral("ready;"));
    const int sampleRateIndex = traceIndex(
        entries, QStringLiteral("client->server"),
        QStringLiteral("audio_samplerate:48000;"));
    const int audioStartIndex = traceIndex(
        entries, QStringLiteral("client->server"),
        QStringLiteral("audio_start:0;"));
    const int splitRequestIndex = traceIndex(
        entries, QStringLiteral("client->server"),
        QStringLiteral("split_enable:0,false;"));
    const int vfoRequestIndex = traceIndex(
        entries, QStringLiteral("client->server"),
        QStringLiteral("vfo:0,1,14076000;"));
    check(readyIndex >= 0 && readyIndex < sampleRateIndex
              && sampleRateIndex < audioStartIndex
              && audioStartIndex < splitRequestIndex
              && splitRequestIndex < vfoRequestIndex,
          "trace preserves ready, negotiation, split, then VFO B ordering");

    response = bridge.request(QByteArrayLiteral("tci routes"));
    check(response.value(QStringLiteral("ok")).toBool()
              && response.value(QStringLiteral("routeOwner")).toString()
                  == QStringLiteral("external")
              && response.value(QStringLiteral("rxSliceId")).toInt() == 4
              && response.value(QStringLiteral("txSliceId")).toInt() == 7,
          "tci routes exposes stable external multi-slice ownership");

    QTemporaryDir tempDir;
    const QString tracePath = tempDir.filePath(QStringLiteral("tci-trace.json"));
    response = bridge.request(
        QByteArrayLiteral("tci trace export ") + tracePath.toUtf8());
    QFile exported(tracePath);
    check(response.value(QStringLiteral("ok")).toBool()
              && exported.open(QIODevice::ReadOnly)
              && QJsonDocument::fromJson(exported.readAll()).object()
                     .value(QStringLiteral("count")).toInt() >= entries.size(),
          "tci trace export atomically writes a reusable JSON oracle");

    automation.setReadOnly(true);
    check(bridge.request(QByteArrayLiteral("tci routes"))
              .value(QStringLiteral("ok")).toBool(),
          "observe-only mode permits tci routes");
    check(bridge.request(QByteArrayLiteral("tci status"))
              .value(QStringLiteral("ok")).toBool(),
          "observe-only mode permits tci status");
    check(!bridge.request(QByteArrayLiteral("tci send trx:0,true,tci"))
               .value(QStringLiteral("ok")).toBool(),
          "observe-only mode blocks tci send");
    automation.setReadOnly(false);

    response = bridge.request(QByteArrayLiteral("tci stop abrupt"));
    check(response.value(QStringLiteral("ok")).toBool()
              && response.value(QStringLiteral("abrupt")).toBool(),
          "tci stop abrupt tears down the simulator");

    peer->deleteLater();
    fakeTci.close();
    automation.stop();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    if (failures != 0) {
        std::fprintf(stderr, "tci_automation_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("tci_automation_test: all checks passed\n");
    return 0;
}
