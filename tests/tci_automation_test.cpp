#include "TestSettingsProfile.h"
#include "TestEventLoop.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>
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
    const bool ok = AetherTest::waitFor(predicate, timeoutMs);
    // Trailing drain, deliberately preserved from the pre-#4699 helper. Call
    // sites below wait on one condition and then assert on state that settles an
    // event hop LATER: the restart check waits for the SERVER to see a pending
    // connection, then asserts the BRIDGE already counts that client as live.
    // waitFor() returns the moment its predicate holds, so without this the hop
    // is lost. Observed: 4 failures of "a live client with the same id is still
    // refused" across 360 runs under 14-way CPU contention, against 0/360 for
    // main's version of this file in the same interleaved run. It did not
    // reproduce in a further 400 runs, so the rate is low and the drain's effect
    // was never demonstrated by a clean A/B — this is kept because it restores
    // exactly what the pre-#4699 helper did, not because it was proven to fix
    // the flake.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return ok;
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

    // ── Two WSJT-X instances on two receivers (#4547) ──────────────────
    // The shape the routing fix exists for: each instance declares its own
    // receiver in audio_start, which is the only per-client signal the TCI wire
    // carries. The single-sim spelling above kept working unchanged, which is
    // the compatibility half of this.
    response = bridge.request(
        QByteArray("tci start ") + QByteArray::number(fakeTci.serverPort())
        + QByteArrayLiteral(" @b rx=1"));
    check(response.value(QStringLiteral("ok")).toBool()
              && response.value(QStringLiteral("client")).toString()
                  == QStringLiteral("b")
              && response.value(QStringLiteral("receiver")).toInt() == 1,
          "a second simulated client starts on its own receiver");

    check(spinUntil([&fakeTci] { return fakeTci.hasPendingConnections(); }),
          "the second simulator connects to the fake TCI server");
    QWebSocket* peerB = fakeTci.nextPendingConnection();
    check(peerB != nullptr, "fake TCI server accepts the second connection");

    QStringList framesB;
    if (peerB) {
        QObject::connect(peerB, &QWebSocket::textMessageReceived,
                         [&framesB](const QString& text) {
            framesB.append(text);
        });
        peerB->sendTextMessage(QStringLiteral(
            "protocol:ExpertSDR3,2.0;channels_count:2;ready;"));
        check(spinUntil([&framesB] {
            return framesB.contains(QStringLiteral("audio_start:1;"));
        }), "the second client declares receiver 1, not receiver 0");
    }

    // Starting the same id twice must be refused rather than silently leaking a
    // socket — the failure mode that made the old single-sim guard necessary.
    check(!bridge.request(
              QByteArray("tci start ") + QByteArray::number(fakeTci.serverPort())
              + QByteArrayLiteral(" @b")).value(QStringLiteral("ok")).toBool(),
          "starting a duplicate client id is refused");

    // `tci send @b` must reach B's socket and only B's.
    const int framesABefore = clientFrames.size();
    response = bridge.request(
        QByteArrayLiteral("tci send @b trx:0,true,tci"));
    check(response.value(QStringLiteral("ok")).toBool()
              && response.value(QStringLiteral("client")).toString()
                  == QStringLiteral("b"),
          "tci send @b addresses the named client");
    check(spinUntil([&framesB] {
        return framesB.contains(QStringLiteral("trx:0,true,tci;"));
    }), "the keyed command reaches the second client's socket");
    check(clientFrames.size() == framesABefore,
          "the first client's socket sees none of it");

    // The transcript has to stay readable with two clients interleaved.
    const QJsonArray twoClientEntries = bridge.request(
        QByteArrayLiteral("tci trace status 200"))
        .value(QStringLiteral("entries")).toArray();
    bool sawA = false;
    bool sawB = false;
    for (const QJsonValue& v : twoClientEntries) {
        const QString who = v.toObject().value(QStringLiteral("client")).toString();
        if (who == QStringLiteral("a")) sawA = true;
        if (who == QStringLiteral("b")) sawB = true;
    }
    check(sawA && sawB, "the trace attributes each frame to its client");

    const QJsonObject bothStatus = bridge.request(QByteArrayLiteral("tci status"));
    check(bothStatus.value(QStringLiteral("clientCount")).toInt() == 2,
          "tci status reports both clients");

    // Abrupt teardown of one named client leaves the other running — the reap
    // path is per-socket, so a shared teardown would have been invisible here.
    response = bridge.request(QByteArrayLiteral("tci stop @a abrupt"));
    check(response.value(QStringLiteral("ok")).toBool()
              && response.value(QStringLiteral("abrupt")).toBool()
              && response.value(QStringLiteral("running")).toBool(),
          "tci stop @a abrupt tears down only that client");

    response = bridge.request(QByteArrayLiteral("tci stop all"));
    check(response.value(QStringLiteral("ok")).toBool()
              && response.value(QStringLiteral("stopped")).toArray().size() == 1
              && !response.value(QStringLiteral("running")).toBool(),
          "tci stop all tears down the remaining simulated client");

    // ── Restart after a server-side close (#4017, regressed by the id list) ──
    // The disconnected lambda reaps the socket but the client entry survives so
    // `tci status` can still report closeReason. Keying "already running" off
    // the id alone therefore refused every restart after the radio or server
    // dropped the connection — the exact failure #4017 fixed. The start path
    // must recycle a slot whose socket is gone.
    response = bridge.request(
        QByteArray("tci start ") + QByteArray::number(fakeTci.serverPort())
        + QByteArrayLiteral(" @c"));
    check(response.value(QStringLiteral("ok")).toBool(),
          "a client starts for the server-close restart check");
    check(spinUntil([&fakeTci] { return fakeTci.hasPendingConnections(); }),
          "it connects to the fake TCI server");
    QWebSocket* peerC = fakeTci.nextPendingConnection();
    check(spinUntil([&bridge] {
        return bridge.request(QByteArrayLiteral("tci status @c"))
            .value(QStringLiteral("connected")).toBool();
    }), "it reports connected before the server closes it");

    if (peerC) peerC->close();          // server-side close, not `tci stop`
    check(spinUntil([&bridge] {
        return !bridge.request(QByteArrayLiteral("tci status @c"))
            .value(QStringLiteral("connected")).toBool();
    }), "the simulator observes the server-side close");
    check(bridge.request(QByteArrayLiteral("tci status @c"))
              .value(QStringLiteral("closeReason")).toString()
              == QStringLiteral("server closed"),
          "the reaped client keeps its closeReason for diagnosis");

    response = bridge.request(
        QByteArray("tci start ") + QByteArray::number(fakeTci.serverPort())
        + QByteArrayLiteral(" @c"));
    check(response.value(QStringLiteral("ok")).toBool(),
          "the same id restarts after a server-side close, not 'already running'");
    check(spinUntil([&fakeTci] { return fakeTci.hasPendingConnections(); }),
          "the restarted client reconnects");
    QWebSocket* peerC2 = fakeTci.nextPendingConnection();

    // A LIVE client with the same id is still refused — recycling the dead slot
    // must not become a way to leak a second socket under one name.
    check(!bridge.request(
              QByteArray("tci start ") + QByteArray::number(fakeTci.serverPort())
              + QByteArrayLiteral(" @c")).value(QStringLiteral("ok")).toBool(),
          "a live client with the same id is still refused");
    bridge.request(QByteArrayLiteral("tci stop all"));

    if (peerC) peerC->deleteLater();
    if (peerC2) peerC2->deleteLater();
    if (peerB) peerB->deleteLater();
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
