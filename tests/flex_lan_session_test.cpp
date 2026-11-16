// Production lifecycle with the built-in synthetic transport. No OS socket is
// connected or bound, no discovery runs, and no radio/firmware peer is emulated.
#include "TestSettingsProfile.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/RadioConnection.h"
#include "core/backends/flex/WanConnection.h"
#include "core/backends/sim/SimBackend.h"
#include "models/RadioModel.h"
#include "core/control/RadioConnectionTarget.h"

#include <QCoreApplication>
#include <QEvent>
#include <QSignalSpy>
#include <QtTest>
#include <atomic>

using namespace AetherSDR;

class CleanupMemorySocket final : public QTcpSocket {
public:
    explicit CleanupMemorySocket(QObject* parent) : QTcpSocket(parent)
    {
        open(ReadWrite);
        setSocketState(ConnectedState);
    }
    void disconnectFromHost() override
    {
        setSocketState(UnconnectedState);
        emit disconnected();
    }
    bool waitForBytesWritten(int) override { return true; }
protected:
    qint64 writeData(const char*, qint64 size) override { return size; }
};

namespace AetherSDR {
class RadioConnectionSessionTestAccess {
public:
    class NoDialSocket final : public QTcpSocket {
    public:
        NoDialSocket(QObject* parent, std::atomic<int>& attempts)
            : QTcpSocket(parent), m_attempts(attempts) {}
        void connectToHost(const QString&, quint16, OpenMode = ReadWrite,
                           NetworkLayerProtocol = AnyIPProtocol) override
        { ++m_attempts; }
    private:
        std::atomic<int>& m_attempts;
    };
    static void installNoDialSocket(RadioConnection& wire, std::atomic<int>& attempts)
    {
        QMetaObject::invokeMethod(&wire, [&wire, &attempts] {
            wire.m_socket->disconnect(&wire);
            wire.m_socket->deleteLater();
            wire.m_socket = new NoDialSocket(&wire, attempts);
        }, Qt::BlockingQueuedConnection);
    }
    static void installCleanupSink(RadioConnection& wire, QStringList& commands, QList<quint32>& sequences)
    {
        wire.m_socket->deleteLater(); // unbound wrapper, on its owner thread
        wire.m_socket = new CleanupMemorySocket(&wire);
        wire.m_syntheticDemo = false;
        QObject::connect(wire.m_socket, &QTcpSocket::disconnected,
                         &wire, &RadioConnection::onSocketDisconnected);
        wire.m_commandSinkForTest = [&wire, &commands, &sequences](quint32 sequence, const QString& command) {
            sequences.append(sequence);
            commands.append(command);
            QTimer::singleShot(0, &wire, [&wire, sequence] { emit wire.commandResponse(sequence, 0, {}); });
        };
    }
    static void endpoints(RadioConnection& wire, const QHostAddress& local)
    {
        wire.m_radioAddr = QHostAddress(QStringLiteral("192.0.2.1"));
        wire.m_localAddr = local;
        wire.m_localPort = 32123;
    }
};
}


namespace {
RadioConnectRequest demoRequest()
{
    RadioConnectRequest request;
    request.serial = SimBackend::demoSerial();
    request.host = QStringLiteral("127.0.0.1");
    request.port = 4992;
    return request;
}

void drain(RadioConnection* wire)
{
    QMetaObject::invokeMethod(wire, [] {}, Qt::BlockingQueuedConnection);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
}

RadioInfo demoInfo()
{
    RadioInfo info;
    info.serial = SimBackend::demoSerial();
    info.family = SimBackend::familyName();
    info.model = SimBackend::demoModelName();
    info.address = QHostAddress::LocalHost;
    info.port = 4992;
    return info;
}
}

class FlexLanSessionTest final : public QObject {
    Q_OBJECT
private slots:
    void handshakeAndIdempotence()
    {
        FlexBackend backend;
        QSignalSpy starting(&backend, &IRadioBackend::connectionSessionStarting);
        QSignalSpy connected(&backend, &IRadioBackend::connected);
        QSignalSpy ended(&backend, &IRadioBackend::disconnected);
        QStringList order;
        connect(&backend, &IRadioBackend::connectionSessionStarting, this, [&] { order << "starting"; });
        connect(&backend, &IRadioBackend::connectionVersionReceived, this, [&] { order << "version"; });
        connect(&backend, &IRadioBackend::connected, this, [&] {
            order << "connected";
            QVERIFY(backend.isConnected());
            QVERIFY(backend.connectionSession());
            QVERIFY(backend.connectionSession()->clientHandle != 0);
        });
        backend.connectRadio(demoRequest());
        backend.connectRadio(demoRequest());
        QCOMPARE(starting.size(), 1);
        QTRY_COMPARE(connected.size(), 1);
        QCOMPARE(order, QStringList({"starting", "version", "connected"}));
        const ConnectionSession first = *backend.connectionSession();
        QCOMPARE(first.epoch, starting.first().first().toULongLong());
        backend.connectRadio(demoRequest());
        QCOMPARE(starting.size(), 1);
        QVERIFY(backend.startReceiveTransport().ready); // synthetic UDP stays idle
        backend.disconnectRadio();
        QVERIFY(!backend.isConnected());
        QVERIFY(!backend.connectionSession());
        QVERIFY(!backend.startReceiveTransport().ready);
        QCOMPARE(ended.size(), 1);
        backend.disconnectRadio();
        drain(backend.connection());
        QCOMPARE(ended.size(), 1);
        backend.connectRadio(demoRequest());
        QTRY_COMPARE(connected.size(), 2);
        QVERIFY(backend.connectionSession()->epoch != first.epoch);
        QCOMPARE(first.epoch, starting.first().first().toULongLong()); // immutable copy
    }

    void gracefulCleanupUsesSharedSequenceAndOriginalStream()
    {
        QStringList commands;
        QList<quint32> sequences;
        FlexBackend backend;
        quint32 nextSequence = 700;
        backend.setIndependentTxSequenceProviderForTest([&] { return nextSequence++; });
        backend.connectRadio(demoRequest());
        QTRY_VERIFY(backend.connectionSession().has_value());
        RadioConnection* wire = backend.connection();
        QMetaObject::invokeMethod(wire, [&] {
            RadioConnectionSessionTestAccess::installCleanupSink(*wire, commands, sequences);
        }, Qt::BlockingQueuedConnection);
        backend.disconnectRadio({0x40000001});
        QCOMPARE(commands, QStringList({"stream remove 0x40000001"}));
        QCOMPARE(sequences, QList<quint32>({700}));
        QCOMPARE(nextSequence, 701u);
        QVERIFY(!backend.isConnected());
    }

    void endpointSnapshotDoesNotBorrowMutableWorkerState()
    {
        FlexBackend backend;
        RadioConnection* wire = backend.connection();
        QMetaObject::invokeMethod(wire, [&] {
            RadioConnectionSessionTestAccess::endpoints(*wire, QHostAddress(QStringLiteral("192.0.2.2")));
        }, Qt::BlockingQueuedConnection);
        backend.connectRadio(demoRequest());
        QTRY_VERIFY(backend.connectionSession().has_value());
        const ConnectionSession state = *backend.connectionSession();
        QCOMPARE(state.localAddress, QHostAddress(QStringLiteral("192.0.2.2")));
        QCOMPARE(state.remoteAddress, QHostAddress(QStringLiteral("192.0.2.1")));
        QCOMPARE(state.localPort, 32123);
        QMetaObject::invokeMethod(wire, [&] {
            RadioConnectionSessionTestAccess::endpoints(*wire, QHostAddress(QStringLiteral("192.0.2.3")));
        }, Qt::BlockingQueuedConnection);
        QCOMPARE(backend.connectionSession()->localAddress, state.localAddress);
        backend.disconnectRadio();
        QVERIFY(!backend.connectionSession());
    }

    void abandonedEventsCannotEndReplacement()
    {
        FlexBackend backend;
        QSignalSpy starting(&backend, &IRadioBackend::connectionSessionStarting);
        QSignalSpy connected(&backend, &IRadioBackend::connected);
        QSignalSpy errors(&backend, &IRadioBackend::connectionError);
        QSignalSpy ended(&backend, &IRadioBackend::disconnected);
        backend.connectRadio(demoRequest());
        QTRY_COMPARE(connected.size(), 1);
        RadioConnection* wire = backend.connection();
        // Producer-side emissions are definitely queued before cancellation;
        // do not drain the owner until the replacement attempt is admitted.
        QMetaObject::invokeMethod(wire, [wire] {
            emit wire->connected();
            emit wire->errorOccurred(QStringLiteral("retired failure"));
            emit wire->disconnected();
            emit wire->versionReceived(QStringLiteral("retired version"));
        }, Qt::BlockingQueuedConnection);
        backend.disconnectRadio();
        backend.connectRadio(demoRequest());
        QTRY_COMPARE(connected.size(), 2);
        drain(wire);
        QVERIFY(backend.isConnected());
        QVERIFY(backend.connectionSession());
        QCOMPARE(backend.connectionSession()->epoch, starting.last().first().toULongLong());
        QCOMPARE(errors.size(), 0);
        QCOMPARE(ended.size(), 1);
    }

    void cancelBeforeHandshake()
    {
        FlexBackend backend;
        QSignalSpy connected(&backend, &IRadioBackend::connected);
        QSignalSpy ended(&backend, &IRadioBackend::disconnected);
        for (int i = 0; i < 8; ++i) {
            backend.connectRadio(demoRequest());
            backend.disconnectRadio();
            drain(backend.connection());
            QVERIFY(!backend.isConnected());
        }
        QCOMPARE(connected.size(), 0);
        QCOMPARE(ended.size(), 8);
        backend.connectRadio(demoRequest());
        QTRY_COMPARE(connected.size(), 1);
    }

    void preHandshakeFailureEndsExactlyOnce()
    {
        std::atomic<int> dialAttempts{0};
        FlexBackend backend;
        RadioConnectionSessionTestAccess::installNoDialSocket(*backend.connection(), dialAttempts);
        QSignalSpy errors(&backend, &IRadioBackend::connectionError);
        QSignalSpy ended(&backend, &IRadioBackend::disconnected);
        RadioConnectRequest request;
        request.host = QStringLiteral("192.0.2.1");
        request.port = 4992;
        request.requireExplicitLocalBind = true;
        request.explicitLocalBindAddress = QHostAddress(QStringLiteral("2001:db8::2"));
        backend.connectRadio(request); // address-family validation rejects before any bind/dial
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(dialAttempts.load(), 0);
        QVERIFY(!backend.connectionSession());
        backend.disconnectRadio();
        QCOMPARE(ended.size(), 1);
        backend.connectRadio(demoRequest());
        QTRY_VERIFY(backend.connectionSession().has_value());
    }

    void reconnectBeforeDisconnectNotification_data()
    {
        QTest::addColumn<bool>("replaceEndpoint");
        QTest::newRow("same-endpoint") << false;
        QTest::newRow("replacement-endpoint") << true;
    }

    void reconnectBeforeDisconnectNotification()
    {
        QFETCH(bool, replaceEndpoint);
        RadioModel model;
        RadioInfo selected = demoInfo();
        selected.family = QStringLiteral("flex");
        QSignalSpy starting(model.backend(), &IRadioBackend::connectionSessionStarting);
        QSignalSpy ended(model.backend(), &IRadioBackend::disconnected);
        model.connectToRadio(selected);
        QTRY_VERIFY(model.backend()->connectionSession().has_value());
        QVERIFY(!model.isConnectAttemptInFlight());
        const quint64 firstEpoch = model.backend()->connectionSession()->epoch;
        RadioConnection* wire = model.connection();
        // End the worker transport without delivering its queued owner notification.
        QMetaObject::invokeMethod(wire, [wire] { wire->disconnectFromRadio(); }, Qt::BlockingQueuedConnection);
        QVERIFY(!model.isConnected());
        QVERIFY(model.backend()->connectionSession());
        if (replaceEndpoint) { selected.address = QHostAddress(QStringLiteral("127.0.0.2")); }

        model.connectToRadio(selected);
        QCOMPARE(starting.size(), 2);
        QCOMPARE(ended.size(), 1);
        QTRY_VERIFY(model.isConnected() && !model.isConnectAttemptInFlight());
        QVERIFY(model.backend()->connectionSession());
        QVERIFY(model.backend()->connectionSession()->epoch != firstEpoch);
        drain(wire);
        QVERIFY(model.isConnected());
        QCOMPARE(ended.size(), 1);
        model.disconnectFromRadio();
    }

    void wanSelectionClosesLanAndRejectsItsQueuedEvents_data()
    {
        QTest::addColumn<bool>("transportEnded");
        QTest::newRow("connected-transport") << false;
        QTest::newRow("ended-transport") << true;
    }

    void wanSelectionClosesLanAndRejectsItsQueuedEvents()
    {
        QFETCH(bool, transportEnded);
        WanConnection wan; // never initialized or connected
        RadioModel model;
        RadioInfo selected = demoInfo();
        selected.family = QStringLiteral("flex"); // exercise the LAN owner with its synthetic wire
        model.connectToRadio(selected);
        QTRY_VERIFY(model.backend()->connectionSession().has_value());
        RadioConnection* wire = model.connection();
        QMetaObject::invokeMethod(wire, [wire, transportEnded] {
            if (transportEnded) { wire->disconnectFromRadio(); }
            else { emit wire->disconnected(); }
        }, Qt::BlockingQueuedConnection);
        QCOMPARE(wire->isConnected(), !transportEnded);
        QVERIFY(model.backend()->connectionSession());
        model.connectViaWan(&wan, QStringLiteral("192.0.2.1"), 4992);
        QVERIFY(model.isWan());
        QVERIFY(!wire->isConnected());
        QVERIFY(!model.backend()->connectionSession());
        drain(wire);
        QVERIFY(model.isWan());
        model.disconnectFromRadio();
    }

    void errorObserverCanReplaceThePendingAttempt()
    {
        std::atomic<int> dialAttempts{0};
        RadioModel model;
        auto* backend = dynamic_cast<FlexBackend*>(model.backend());
        QVERIFY(backend);
        RadioConnectionSessionTestAccess::installNoDialSocket(*backend->connection(), dialAttempts);
        bool replaced = false;
        connect(&model, &RadioModel::connectionError, this, [&](const QString&) {
            if (replaced) { return; }
            replaced = true;
            RadioInfo selected = demoInfo();
            selected.family = QStringLiteral("flex");
            model.connectToRadio(selected);
        });
        RadioInfo invalid;
        invalid.family = QStringLiteral("flex");
        invalid.serial = QStringLiteral("invalid-source");
        invalid.address = QHostAddress(QStringLiteral("192.0.2.1"));
        invalid.port = 4992;
        invalid.bindSettings.mode = RadioBindMode::Explicit;
        invalid.bindSettings.bindAddress = QHostAddress(QStringLiteral("2001:db8::2"));
        model.connectToRadio(invalid);
        QTRY_VERIFY_WITH_TIMEOUT(model.backend()->connectionSession().has_value(), 1000);
        QVERIFY(replaced);
        QCOMPARE(dialAttempts.load(), 0);
        QVERIFY(model.isConnected());
        model.disconnectFromRadio();
    }

    void disconnectObserverCanSelectWan()
    {
        WanConnection wan;
        RadioModel model;
        RadioInfo selected = demoInfo();
        selected.family = QStringLiteral("flex");
        model.connectToRadio(selected);
        QTRY_VERIFY(model.backend()->connectionSession().has_value());
        bool selectedWan = false;
        connect(&model, &RadioModel::connectionStateChanged, this, [&](bool connected) {
            if (!connected && !selectedWan) {
                selectedWan = true;
                model.connectViaWan(&wan, QStringLiteral("192.0.2.1"), 4992);
            }
        });
        model.disconnectFromRadio();
        QVERIFY(selectedWan);
        QVERIFY(model.isWan());
        model.disconnectFromRadio();
    }

    void cancellationFromStartingSignal()
    {
        FlexBackend backend;
        QSignalSpy connected(&backend, &IRadioBackend::connected);
        QSignalSpy ended(&backend, &IRadioBackend::disconnected);
        connect(&backend, &IRadioBackend::connectionSessionStarting, this,
                [&] { backend.disconnectRadio(); });
        backend.connectRadio(demoRequest());
        drain(backend.connection());
        QCOMPARE(ended.size(), 1);
        QCOMPARE(connected.size(), 0);
        QVERIFY(!backend.connectionSession());
    }

    void desktopRetriesButHeadlessRequiresIntent()
    {
        RadioModel desktop;
        RadioModel daemon;
        auto target = control::makeModelRadioConnectionTarget(&daemon);
        QVERIFY(target);
        DiscoveredRadio selected;
        selected.family = SimBackend::familyName();
        selected.serial = SimBackend::demoSerial();
        selected.transport = QStringLiteral("sim");
        selected.address = QStringLiteral("127.0.0.1");
        selected.port = 4992;
        desktop.connectToRadio(demoInfo());
        target->connectRadio(selected);
        QTRY_VERIFY(desktop.isConnected());
        QTRY_COMPARE(target->state(), control::RadioConnectionTarget::State::Connected);
        desktop.forceDisconnect();
        daemon.forceDisconnect();
        QTRY_COMPARE(target->state(), control::RadioConnectionTarget::State::Idle);
        QCOMPARE(target->errorCode(), QStringLiteral("engine.failed"));
        QTRY_VERIFY_WITH_TIMEOUT(desktop.isConnected(), 7000);
        QVERIFY(!daemon.isConnected());
        QCOMPARE(target->state(), control::RadioConnectionTarget::State::Idle);
        target->connectRadio(selected);
        QTRY_COMPARE(target->state(), control::RadioConnectionTarget::State::Connected);
        QVERIFY(target->errorCode().isEmpty());
        target->disconnectRadio();
        QTRY_COMPARE(target->state(), control::RadioConnectionTarget::State::Idle);
        desktop.disconnectFromRadio();
    }

    void modelDemoUsesOneLifecycle()
    {
        RadioModel model;
        QSignalSpy changes(&model, &RadioModel::connectionStateChanged);
        const RadioInfo info = demoInfo();
        model.connectToRadio(info);
        QTRY_VERIFY(model.isConnected());
        QTRY_VERIFY(model.slice(0) != nullptr);
        QCOMPARE(changes.size(), 1);
        model.connectToRadio(info);
        drain(model.connection());
        QCOMPARE(changes.size(), 1);
        model.disconnectFromRadio();
        QVERIFY(!model.isConnected());
        QCOMPARE(changes.size(), 2);
        QVERIFY(!changes.last().first().toBool());
        model.connectToRadio(info);
        QTRY_VERIFY(model.isConnected());
        QCOMPARE(changes.size(), 3);
        model.disconnectFromRadio();
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    TestSettingsProfile profile(QStringLiteral("flex-lan-session"));
    if (!profile.isValid()) { return 2; }
    FlexLanSessionTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "flex_lan_session_test.moc"
