#include "TestSettingsProfile.h"
#include "core/CatPort.h"
#include "core/backends/flex/FlexBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTest>

#include <memory>

using namespace AetherSDR;

class ConnectedOtrspBackend final : public FlexBackend {
public:
    bool isConnected() const override { return true; }
};

class OtrspPortTest : public QObject {
    Q_OBJECT

private slots:
    void connectionLifecycle()
    {
        CatPort port(nullptr);
        port.setDialect(CatDialect::OTRSP);
        port.setVfoA(2);
        port.setVfoB(5);
        QVERIFY(port.start(0));
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

        QVERIFY(port.start(0));
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
    }

    void focusDispatch()
    {
        QStringList commands;
        RadioModel radio;
        auto owned = std::make_unique<ConnectedOtrspBackend>();
        ConnectedOtrspBackend* backend = owned.get();
        backend->setSliceCommandSink([&](const QString& command) { commands.append(command); });
        backend->setCommandSink([&](const QString& command) { commands.append(command); });
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
        QVERIFY(port.start(0));
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, port.port());
        QTRY_COMPARE_WITH_TIMEOUT(port.clientCount(), 1, 2000);
        client.write("RX2R\rTX1\r");
        QTRY_VERIFY_WITH_TIMEOUT(commands.contains("slice set 6 tx=1"), 2000);
        QVERIFY(commands.contains("slice set 6 audio_pan=100"));
        QVERIFY(commands.contains("slice set 3 audio_pan=0"));
        QVERIFY(commands.contains("slice set 6 audio_mute=0"));
        QVERIFY(commands.contains("slice set 3 audio_mute=0"));
        QVERIFY(commands.contains("slice set 3 active=1"));
        QCOMPARE(commands.size(), 6);
        QVERIFY(!radio.transmitModel().isTransmitting());
        commands.clear();
        QObject::connect(radio.slice(6), &SliceModel::audioPanChanged, &port,
                         [&](int) { port.stop(); });
        client.write("RX1S\rTX2\r");
        QTRY_VERIFY_WITH_TIMEOUT(!port.isRunning(), 2000);
        QCOMPARE(commands, QStringList{"slice set 6 audio_pan=0"});
        QCOMPARE(port.clientCount(), 0);
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
    return QTest::qExec(&test, argc, argv);
}

#include "otrsp_port_test.moc"
