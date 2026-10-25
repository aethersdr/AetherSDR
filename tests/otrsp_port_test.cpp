#include "TestSettingsProfile.h"
#include "core/CatPort.h"
#include "core/backends/flex/FlexBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTcpServer>
#include <QTest>

#include <memory>

using namespace AetherSDR;

class ConnectedOtrspBackend final : public FlexBackend {
public:
    bool isConnected() const override { return true; }
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps = FlexBackend::capabilities();
        caps.hasMultiClientSessions = false;
        return caps;
    }
};

class OtrspPortTest : public QObject {
    Q_OBJECT

public:
    bool bindSkipped() const { return m_bindSkipped; }

private:
    bool m_bindSkipped = false;

private slots:
    void dialectKeys()
    {
        for (CatDialect dialect : {CatDialect::Rigctld, CatDialect::TS2000,
                                   CatDialect::FlexCAT, CatDialect::OTRSP}) {
            QCOMPARE(dialectFromKey(dialectKey(dialect)), dialect);
        }
        QCOMPARE(dialectFromKey(QStringLiteral("unknown")), CatDialect::Rigctld);
    }

    void bindFailureReported()
    {
        QTcpServer occupied;
        if (!occupied.listen(QHostAddress::Any, 0)) {
            m_bindSkipped = true;
            QSKIP("Cannot bind ephemeral TCP port");
        }
        CatPort port(nullptr);
        port.setDialect(CatDialect::OTRSP);
        port.setVfoA(0);
        port.setVfoB(1);
        QSignalSpy state(&port, &CatPort::stateChanged);
        QVERIFY(!port.start(occupied.serverPort()));
        QVERIFY(!port.errorString().isEmpty());
        QVERIFY(!port.isRunning());
        QCOMPARE(state.size(), 1);
        occupied.close();
        if (!port.start(0)) {
            m_bindSkipped = true;
            QSKIP("Cannot bind ephemeral TCP port after releasing occupied listener");
        }
        QVERIFY(port.errorString().isEmpty());
        QVERIFY(port.isRunning());
        port.stop();
    }

    void connectionLifecycle()
    {
        CatPort port(nullptr);
        port.setDialect(CatDialect::OTRSP);
        port.setVfoA(2);
        port.setVfoB(5);
        if (!port.start(0)) {
            m_bindSkipped = true;
            QSKIP("Cannot bind ephemeral TCP port");
        }
        QVERIFY(port.isRunning());
        QVERIFY(port.port() != 0);
        QSignalSpy counts(&port, &CatPort::clientCountChanged);
        QTcpSocket first;
        QTcpSocket second;
        first.connectToHost(QHostAddress::LocalHost, port.port());
        second.connectToHost(QHostAddress::LocalHost, port.port());
        QTRY_COMPARE_WITH_TIMEOUT(port.clientCount(), 2, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(first.state(), QAbstractSocket::ConnectedState, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(second.state(), QAbstractSocket::ConnectedState, 2000);
        first.write("?NA");
        second.write("?NAME\r?\r");
        QTRY_COMPARE_WITH_TIMEOUT(second.bytesAvailable(), 16, 2000);
        QCOMPARE(second.readAll(), QByteArray("NAMEAetherSDR\r?\r"));
        QCOMPARE(first.bytesAvailable(), 0);
        first.write("ME\r");
        QTRY_COMPARE_WITH_TIMEOUT(first.bytesAvailable(), 14, 2000);
        QCOMPARE(first.readAll(), QByteArray("NAMEAetherSDR\r"));
        port.stop();
        QCOMPARE(port.clientCount(), 0);
        QVERIFY(!port.isRunning());
        QCOMPARE(port.port(), quint16(0));
        QTRY_COMPARE_WITH_TIMEOUT(first.state(), QAbstractSocket::UnconnectedState, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(second.state(), QAbstractSocket::UnconnectedState, 2000);
        QVERIFY(!counts.isEmpty());
        QCOMPARE(counts.last().first().toInt(), 0);

        if (!port.start(0)) {
            m_bindSkipped = true;
            QSKIP("Cannot bind ephemeral TCP port after restart");
        }
        QTcpSocket restarted;
        restarted.connectToHost(QHostAddress::LocalHost, port.port());
        QTRY_COMPARE_WITH_TIMEOUT(port.clientCount(), 1, 2000);
        restarted.write("?NAME\r");
        QTRY_COMPARE_WITH_TIMEOUT(restarted.bytesAvailable(), 14, 2000);
        QCOMPARE(restarted.readAll(), QByteArray("NAMEAetherSDR\r"));
        restarted.disconnectFromHost();
        QTRY_COMPARE_WITH_TIMEOUT(port.clientCount(), 0, 2000);
        port.stop();
    }

    void invalidMappings()
    {
        CatPort port(nullptr);
        port.setDialect(CatDialect::OTRSP);
        QVERIFY(!port.start(0));
        port.setVfoB(0);
        QVERIFY(!port.start(0));
        QVERIFY(!port.isRunning());
        port.setVfoB(8);
        QVERIFY(!port.start(0));
        QVERIFY(!port.errorString().isEmpty());
    }

    void focusDispatch_data()
    {
        QTest::addColumn<bool>("disconnectFirst");
        QTest::newRow("stop-during-dispatch") << false;
        QTest::newRow("disconnect-then-stop-during-dispatch") << true;
    }

    void focusDispatch()
    {
        QFETCH(bool, disconnectFirst);
        QStringList commands;
        RadioModel radio;
        auto owned = std::make_unique<ConnectedOtrspBackend>();
        ConnectedOtrspBackend* backend = owned.get();
        backend->setSliceCommandSink([&](const QString& command) { commands.append(command); });
        backend->setCommandSink([&](const QString& command) { commands.append(command); });
        backend->setSetupCommandSink([&](const QString& command, const RadioSetupCompletion&) {
            commands.append(command);
            return true;
        });
        radio.setBackendForTest(std::move(owned), QStringLiteral("otrsp-port-test"));
        for (int id : {3, 6}) {
            SliceDelta delta;
            delta.inUse = true;
            delta.frequency = 14.2;
            delta.mode = QStringLiteral("USB");
            delta.audioPan = 50;
            delta.audioMute = true;
            delta.active = id == 6;
            delta.txSlice = id == 3;
            emit backend->sliceChanged(id, delta);
        }
        commands.clear();
        CatPort port(&radio);
        port.setDialect(CatDialect::OTRSP);
        port.setVfoA(6);
        port.setVfoB(3);
        if (!port.start(0)) {
            m_bindSkipped = true;
            QSKIP("Cannot bind ephemeral TCP port");
        }
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, port.port());
        QTRY_COMPARE_WITH_TIMEOUT(port.clientCount(), 1, 2000);
        client.write("RX2R\rTX1\r?TX\r?NAME\r");
        QTRY_VERIFY_WITH_TIMEOUT(commands.contains("slice set 6 tx=1"), 2000);
        QVERIFY(commands.contains("slice set 6 audio_pan=100"));
        QVERIFY(commands.contains("slice set 3 audio_pan=0"));
        QVERIFY(commands.contains("slice set 6 audio_mute=0"));
        QVERIFY(commands.contains("slice set 3 audio_mute=0"));
        QVERIFY(commands.contains("slice set 3 active=1"));
        QCOMPARE(commands.size(), 6);
        QCOMPARE(client.bytesAvailable(), 0);
        SliceDelta oldTx;
        oldTx.txSlice = false;
        emit backend->sliceChanged(3, oldTx);
        QCOMPARE(client.bytesAvailable(), 0);
        SliceDelta newTx;
        newTx.txSlice = true;
        emit backend->sliceChanged(6, newTx);
        QTRY_COMPARE_WITH_TIMEOUT(client.bytesAvailable(), 18, 2000);
        QCOMPARE(client.readAll(), QByteArray("TX1\rNAMEAetherSDR\r"));
        QVERIFY(!radio.transmitModel().isTransmitting());
        commands.clear();
        const QPointer<QTcpSocket> serverSocket = port.findChild<QTcpSocket*>();
        QVERIFY(serverSocket);
        bool socketSurvivedStop = false;
        QObject::connect(radio.slice(6), &SliceModel::audioPanChanged, &port, [&](int) {
            if (disconnectFirst) { serverSocket->abort(); }
            port.stop();
            socketSurvivedStop = serverSocket && !serverSocket->parent();
        });
        client.write("RX1S\rTX2\r");
        QTRY_VERIFY_WITH_TIMEOUT(!port.isRunning(), 2000);
        QCOMPARE(commands, QStringList{"slice set 6 audio_pan=0"});
        QCOMPARE(port.clientCount(), 0);
        QVERIFY(socketSurvivedStop);
        QTRY_VERIFY_WITH_TIMEOUT(serverSocket.isNull(), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), QAbstractSocket::UnconnectedState, 2000);
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("otrsp-port"));
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) {
        return 2;
    }
    OtrspPortTest test;
    const int result = QTest::qExec(&test, argc, argv);
    return result == 0 && test.bindSkipped() ? 77 : result;
}

#include "otrsp_port_test.moc"
