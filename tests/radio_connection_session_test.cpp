// Production line assembly with in-memory transport injection. No descriptor,
// listener, network peer, discovery, or radio is used by this test.
#include "core/RadioConnection.h"
#include "core/backends/flex/FlexPttWireSession.h"

#include <QSignalSpy>
#include <QtTest>
#include <algorithm>
#include <cstring>

using namespace AetherSDR;

class MemorySocket final : public QTcpSocket {
public:
    explicit MemorySocket(QObject* parent) : QTcpSocket(parent) { open(ReadWrite); }
    QByteArray input;
    int connectAttempts{0};
    // QAbstractSocket's address overload dispatches to this virtual overload.
    // Stop at the transport boundary: never call the OS socket implementation.
    void connectToHost(const QString&, quint16, OpenMode mode = ReadWrite,
                       NetworkLayerProtocol = AnyIPProtocol) override
    {
        ++connectAttempts;
        open(mode);
    }
    qint64 bytesAvailable() const override { return input.size() + QTcpSocket::bytesAvailable(); }
protected:
    qint64 readData(char* data, qint64 size) override
    {
        const qint64 count = std::min(size, qint64(input.size()));
        std::memcpy(data, input.constData(), size_t(count));
        input.remove(0, count);
        return count;
    }
};

namespace AetherSDR {
class RadioConnectionSessionTestAccess {
public:
    static MemorySocket* attach(RadioConnection& connection)
    {
        auto* socket = new MemorySocket(&connection);
        connection.m_socket = socket;
        return socket;
    }
    static void feed(RadioConnection& connection, MemorySocket& socket, const QByteArray& bytes)
    {
        socket.input += bytes;
        connection.onReadyRead();
    }
    static void remoteDisconnect(RadioConnection& connection) { connection.onSocketDisconnected(); }
    static void failedDisconnect(RadioConnection& connection)
    {
        connection.onSocketError(QAbstractSocket::RemoteHostClosedError);
    }
    static void staleDisconnectedState(RadioConnection& connection)
    {
        connection.m_state.store(ConnectionState::Disconnected);
    }
    static void beginDemo(RadioConnection& connection) { connection.startSyntheticDemoConnect(); }
    static quint64 sessionGeneration(const RadioConnection& connection)
    {
        return connection.m_sessionGeneration;
    }
    static void pendingPing(RadioConnection& connection)
    {
        connection.m_lastPingSeq = 42;
        connection.m_pingStopwatch.start();
    }
    static void beginPtt(RadioConnection& connection, QStringList& writes)
    {
        connection.resetSessionState();
        connection.m_independentPtt = std::make_unique<FlexPttWireSession>(
            [&writes](quint32, const QString& command) { writes.append(command); return true; },
            [](const TxStopEvidence&) {});
    }
};
}

class RadioConnectionSessionTest final : public QObject {
    Q_OBJECT
private slots:
    void whitespaceOnlyLinesAreIgnored()
    {
        RadioConnection connection;
        MemorySocket* socket = RadioConnectionSessionTestAccess::attach(connection);
        QStringList writes;
        RadioConnectionSessionTestAccess::beginPtt(connection, writes);
        QSignalSpy messages(&connection, &RadioConnection::messageReceived);
        RadioConnectionSessionTestAccess::feed(connection, *socket, "\n\r\n \n\t\r\n \t \n");
        QCOMPARE(messages.count(), 0);
        RadioConnectionSessionTestAccess::feed(connection, *socket,
            "V1.4.0.0\nH12345678\nS0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1\n");
        QVERIFY(connection.independentPttReady());
        QCOMPARE(messages.count(), 3);
        RadioConnectionSessionTestAccess::feed(connection, *socket, " \t \r\n");
        QVERIFY(connection.independentPttReady());
        QCOMPARE(messages.count(), 3);
        QCOMPARE(socket->socketDescriptor(), qintptr(-1));
    }
    void partialInterlockRecoversWithoutReconnect()
    {
        RadioConnection connection;
        MemorySocket* socket = RadioConnectionSessionTestAccess::attach(connection);
        QStringList writes;
        RadioConnectionSessionTestAccess::beginPtt(connection, writes);
        RadioConnectionSessionTestAccess::feed(connection, *socket,
            "V1.4.0.0\nH12345678\nS0|interlock tx_allowed=0\n");
        QVERIFY(!connection.independentPttReady());
        const QByteArray idle = "S0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1\n";
        RadioConnectionSessionTestAccess::feed(connection, *socket, idle);
        QVERIFY(connection.independentPttReady());
        RadioConnectionSessionTestAccess::feed(connection, *socket, "S0|interlock tx_allowed=1\n");
        QVERIFY(!connection.independentPttReady());
        RadioConnectionSessionTestAccess::feed(connection, *socket, idle);
        QVERIFY(connection.independentPttReady());
        QVERIFY(writes.isEmpty());
        QCOMPARE(socket->socketDescriptor(), qintptr(-1));
    }
    void independentPttPrologue_data()
    {
        QTest::addColumn<QByteArray>("prologue");
        QTest::addColumn<bool>("supported");
        QTest::newRow("shared-api") << QByteArray("V1.4.0.0\nH12345678\n") << true;
        QTest::newRow("crlf") << QByteArray("V1.4.0.0\r\nH12345678\r\n") << true;
        QTest::newRow("developer-version") << QByteArray("V1.4.7.99\nH12345678\n") << true;
        QTest::newRow("older-api") << QByteArray("V1.3.0.0\nH12345678\n") << false;
        QTest::newRow("future-api") << QByteArray("V2.0.0.0\nH12345678\n") << false;
        QTest::newRow("missing-version") << QByteArray("H12345678\n") << false;
        QTest::newRow("trailing-junk") << QByteArray("V1.4.0.0beta\nH12345678\n") << false;
        QTest::newRow("leading-space") << QByteArray(" V1.4.0.0\nH12345678\n") << false;
        QTest::newRow("trailing-space") << QByteArray("V1.4.0.0 \nH12345678\n") << false;
        QTest::newRow("late-version") << QByteArray("H12345678\nV1.4.0.0\n") << false;
        QTest::newRow("duplicate-version") << QByteArray("V1.4.0.0\nV1.4.0.0\nH12345678\n") << false;
        QTest::newRow("duplicate-handle") << QByteArray("V1.4.0.0\nH12345678\nH12345678\n") << false;
        QTest::newRow("signed-handle") << QByteArray("V1.4.0.0\nH+1234567\n") << false;
        QTest::newRow("zero-handle") << QByteArray("V1.4.0.0\nH00000000\n") << false;
    }
    void independentPttPrologue()
    {
        QFETCH(QByteArray, prologue);
        QFETCH(bool, supported);
        RadioConnection connection;
        MemorySocket* socket = RadioConnectionSessionTestAccess::attach(connection);
        QStringList writes;
        RadioConnectionSessionTestAccess::beginPtt(connection, writes);
        RadioConnectionSessionTestAccess::feed(connection, *socket, prologue);
        QCOMPARE(connection.independentPttSupported(), supported);
        QVERIFY(!connection.independentPttReady()); // protocol alone is not idle proof
        RadioConnectionSessionTestAccess::feed(connection, *socket,
            "S0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1\n");
        QCOMPARE(connection.independentPttReady(), supported);
        const qint64 now = TxCoordinator::monotonicMs();
        TxCoordinator coordinator([](const auto&, auto) {});
        const auto actor = coordinator.registerActor({true, 10000, now + 100000, 1, true});
        const auto operation = coordinator.acquire(actor, now).operation;
        connection.writeIndependentPtt(101, {operation, true});
        QCOMPARE(writes.size(), supported ? 1 : 0);
        QCOMPARE(socket->socketDescriptor(), qintptr(-1));
    }
    void independentPttDoesNotCarryAcrossSessions()
    {
        RadioConnection connection;
        MemorySocket* socket = RadioConnectionSessionTestAccess::attach(connection);
        QStringList writes;
        RadioConnectionSessionTestAccess::beginPtt(connection, writes);
        RadioConnectionSessionTestAccess::feed(connection, *socket, "V1.4.0.0\nH12345678\n");
        QVERIFY(connection.independentPttSupported());
        // The in-memory socket never enters an OS ConnectedState, so inject
        // its disconnect callback explicitly before starting the next session.
        RadioConnectionSessionTestAccess::remoteDisconnect(connection);
        QVERIFY(!connection.independentPttSupported());
        connection.connectToHost(QHostAddress(QHostAddress::LocalHost));
        RadioConnectionSessionTestAccess::feed(connection, *socket,
            "H12345678\nS0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1\n");
        QVERIFY(!connection.independentPttSupported());
        QVERIFY(!connection.independentPttReady());
        RadioConnectionSessionTestAccess::remoteDisconnect(connection);
        connection.connectToHost(QHostAddress(QHostAddress::LocalHost));
        RadioConnectionSessionTestAccess::feed(connection, *socket, "V1.4.0.0\nH12345678\n");
        QVERIFY(connection.independentPttSupported());
        RadioConnectionSessionTestAccess::beginDemo(connection);
        QVERIFY(!connection.independentPttSupported());
        connection.disconnectFromRadio();
        QCOMPARE(socket->socketDescriptor(), qintptr(-1));
    }
    void independentPttProtocolLossRetainsCleanup()
    {
        RadioConnection connection;
        MemorySocket* socket = RadioConnectionSessionTestAccess::attach(connection);
        QStringList writes;
        RadioConnectionSessionTestAccess::beginPtt(connection, writes);
        RadioConnectionSessionTestAccess::feed(connection, *socket,
            "V1.4.0.0\nH12345678\nS0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1\n");
        const qint64 now = TxCoordinator::monotonicMs();
        TxCoordinator coordinator([](const auto&, auto) {});
        const auto actor = coordinator.registerActor({true, 10000, now + 100000, 1, true});
        const auto operation = coordinator.acquire(actor, now).operation;
        connection.writeIndependentPtt(101, {operation, true});
        QCOMPARE(writes, QStringList{"xmit 1"});
        RadioConnectionSessionTestAccess::feed(connection, *socket, "V2.0.0.0\n");
        QVERIFY(!connection.independentPttSupported());
        QVERIFY(!connection.independentPttReady());
        connection.writeIndependentPtt(102, {operation, true});
        QCOMPARE(writes, QStringList{"xmit 1"});
        QVERIFY(coordinator.cancel(actor, operation));
        connection.stopIndependentPtt(103, operation, coordinator.requestStopConfirmation(operation));
        QCOMPARE(writes, (QStringList{"xmit 1", "xmit 0"}));
        QVERIFY(coordinator.recovering());
        QCOMPARE(socket->socketDescriptor(), qintptr(-1));
    }
    void independentPttRequiresLiveTransmitEligibility()
    {
        for (const QByteArray& status : {
                 QByteArray("S0|interlock tx_client_handle=0x00000000 state=RECEIVE reason= source= tx_allowed=0\n"),
                 QByteArray("S0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=0\n"),
                 QByteArray("S0|interlock tx_client_handle=0x87654321 state=TRANSMITTING reason= source=SW tx_allowed=1\n"),
                 QByteArray("S0|interlock state=READY\n")}) {
            RadioConnection connection;
            MemorySocket* socket = RadioConnectionSessionTestAccess::attach(connection);
            QStringList writes;
            RadioConnectionSessionTestAccess::beginPtt(connection, writes);
            RadioConnectionSessionTestAccess::feed(connection, *socket,
                QByteArray("V1.4.0.0\nH12345678\n") + status);
            QVERIFY(connection.independentPttSupported());
            QVERIFY(!connection.independentPttReady());
            const qint64 now = TxCoordinator::monotonicMs();
            TxCoordinator coordinator([](const auto&, auto) {});
            const auto actor = coordinator.registerActor({true, 10000, now + 100000, 1, true});
            const auto operation = coordinator.acquire(actor, now).operation;
            connection.writeIndependentPtt(101, {operation, true});
            QVERIFY(writes.isEmpty());
            QCOMPARE(socket->socketDescriptor(), qintptr(-1));
        }
    }
    void partialLineAcrossDisconnect_data()
    {
        QTest::addColumn<QByteArray>("partial");
        QTest::addColumn<int>("boundary");
        for (const QByteArray& partial : {QByteArray("Vold"), QByteArray("H"),
                 QByteArray("R42|"), QByteArray("S123|radio nickname=old"), QByteArray("Mold")}) {
            for (int boundary = 0; boundary < 3; ++boundary) {
                QTest::newRow((partial.left(1) + QByteArray::number(boundary)).constData())
                    << partial << boundary;
            }
        }
    }
    void partialLineAcrossDisconnect()
    {
        QFETCH(QByteArray, partial);
        QFETCH(int, boundary);
        RadioConnection connection;
        MemorySocket* socket = RadioConnectionSessionTestAccess::attach(connection);
        auto feed = [&](const QByteArray& bytes) {
            RadioConnectionSessionTestAccess::feed(connection, *socket, bytes);
        };
        feed("V1.4.0.0\nH00000001\n");
        RadioConnectionSessionTestAccess::pendingPing(connection);
        QSignalSpy versions(&connection, &RadioConnection::versionReceived);
        QSignalSpy connected(&connection, &RadioConnection::connected);
        QSignalSpy responses(&connection, &RadioConnection::commandResponse);
        QSignalSpy statuses(&connection, &RadioConnection::statusReceived);
        QSignalSpy pings(&connection, &RadioConnection::pingRttMeasured);
        feed(partial);
        if (boundary == 0) {
            RadioConnectionSessionTestAccess::remoteDisconnect(connection);
        } else if (boundary == 1) {
            connection.disconnectFromRadio();
        } else {
            RadioConnectionSessionTestAccess::failedDisconnect(connection);
        }
        QCOMPARE(connection.clientHandle(), 0u);
        // Split a valid new handshake to also pin same-session accumulation.
        feed("V1.4.");
        QCOMPARE(versions.size(), 0);
        feed("0.0\nH00AB");
        QCOMPARE(versions.size(), 1);
        QCOMPARE(versions.at(0).at(0).toString(), QString("1.4.0.0"));
        QCOMPARE(connected.size(), 0);
        feed("CDEF\n");
        QCOMPARE(connected.size(), 1);
        QCOMPARE(connection.clientHandle(), 0x00ABCDEFu);
        QCOMPARE(responses.size(), 0);
        QCOMPARE(statuses.size(), 0);
        feed("R42|0|late\n");
        QCOMPARE(pings.size(), 0); // an old ping must not measure the new session
        QCOMPARE(socket->socketDescriptor(), qintptr(-1));
    }
    void newTcpSessionResetsOldBytes()
    {
        RadioConnection connection;
        MemorySocket* socket = RadioConnectionSessionTestAccess::attach(connection);
        RadioConnectionSessionTestAccess::feed(connection, *socket, "H00000001\nR42|");
        QCOMPARE(connection.clientHandle(), 1u);
        // Seed a disconnected transport with stale parser/handle state so the
        // begin boundary is tested independently of disconnect cleanup.
        RadioConnectionSessionTestAccess::staleDisconnectedState(connection);
        RadioConnectionSessionTestAccess::pendingPing(connection);
        connection.connectToHost(QHostAddress(QHostAddress::LocalHost));
        QCOMPARE(socket->connectAttempts, 1);
        QCOMPARE(connection.clientHandle(), 0u);
        QSignalSpy versions(&connection, &RadioConnection::versionReceived);
        QSignalSpy connected(&connection, &RadioConnection::connected);
        QSignalSpy responses(&connection, &RadioConnection::commandResponse);
        QSignalSpy pings(&connection, &RadioConnection::pingRttMeasured);
        RadioConnectionSessionTestAccess::feed(connection, *socket,
            "V1.4.0.0\nH00ABCDEF\nR42|0|late\n");
        QCOMPARE(versions.size(), 1);
        QCOMPARE(connected.size(), 1);
        QCOMPARE(connection.clientHandle(), 0x00ABCDEFu);
        QCOMPARE(responses.size(), 1);
        QCOMPARE(pings.size(), 0);
        QCOMPARE(socket->socketDescriptor(), qintptr(-1));
    }
    void staleDemoTimersCannotReplayIntoANewSession()
    {
        // The queued synthetic handshake used to test only m_syntheticDemo, which
        // a fast reconnect sets straight back to true — so the old session's
        // timers replayed version/connected/status into the NEW session.
        RadioConnection connection;
        RadioConnectionSessionTestAccess::attach(connection);
        QSignalSpy versions(&connection, &RadioConnection::versionReceived);
        QSignalSpy connected(&connection, &RadioConnection::connected);

        RadioConnectionSessionTestAccess::beginDemo(connection);
        const quint64 first = RadioConnectionSessionTestAccess::sessionGeneration(connection);
        connection.disconnectFromRadio();          // tears the first session down
        RadioConnectionSessionTestAccess::beginDemo(connection);   // ...and a new one starts
        const quint64 second = RadioConnectionSessionTestAccess::sessionGeneration(connection);
        QVERIFY(second != first);                  // the sessions are distinguishable

        // Let every queued handshake timer from BOTH sessions run.
        QTest::qWait(150);

        // Exactly one handshake, the live session's — not two.
        QCOMPARE(versions.size(), 1);
        QCOMPARE(connected.size(), 1);
        connection.disconnectFromRadio();
    }
    void newDemoSessionResetsOldBytes()
    {
        RadioConnection connection;
        MemorySocket* socket = RadioConnectionSessionTestAccess::attach(connection);
        RadioConnectionSessionTestAccess::feed(connection, *socket, "R42|");
        RadioConnectionSessionTestAccess::pendingPing(connection);
        RadioConnectionSessionTestAccess::beginDemo(connection);
        QSignalSpy versions(&connection, &RadioConnection::versionReceived);
        QSignalSpy responses(&connection, &RadioConnection::commandResponse);
        QSignalSpy pings(&connection, &RadioConnection::pingRttMeasured);
        RadioConnectionSessionTestAccess::feed(connection, *socket, "V1.4.0.0\nR42|0|late\n");
        QCOMPARE(versions.size(), 1);
        QCOMPARE(responses.size(), 1);
        QCOMPARE(pings.size(), 0);
        connection.disconnectFromRadio(); // cancel the queued synthetic handshake
    }
};

QTEST_GUILESS_MAIN(RadioConnectionSessionTest)
#include "radio_connection_session_test.moc"
