// Production line assembly with in-memory transport injection. No descriptor,
// listener, network peer, discovery, or radio is used by this test.
#include "core/RadioConnection.h"

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
    static void pendingPing(RadioConnection& connection)
    {
        connection.m_lastPingSeq = 42;
        connection.m_pingStopwatch.start();
    }
};
}

class RadioConnectionSessionTest final : public QObject {
    Q_OBJECT
private slots:
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
