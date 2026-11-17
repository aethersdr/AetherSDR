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
#include <QEventLoop>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>

#include <atomic>
#include <memory>
#include <utility>

using namespace AetherSDR;

struct CleanupGate {
    std::atomic<bool> entered{false};
    std::atomic<bool> released{false};
    std::atomic<bool> finished{false};
};

class CleanupMemorySocket final : public QTcpSocket {
public:
    explicit CleanupMemorySocket(QObject* parent, std::shared_ptr<CleanupGate> gate = {})
        : QTcpSocket(parent), m_gate(std::move(gate))
    {
        open(ReadWrite);
        setSocketState(ConnectedState);
    }
    void disconnectFromHost() override
    {
        if (m_gate) {
            m_gate->entered = true;
            QEventLoop loop;
            QTimer release;
            QObject::connect(&release, &QTimer::timeout, &loop, [&] {
                if (m_gate->released) { loop.quit(); }
            });
            release.start(1);
            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
            loop.exec();
            m_gate->finished = true;
        }
        setSocketState(UnconnectedState);
        emit disconnected();
    }
    bool waitForBytesWritten(int) override { return true; }
protected:
    qint64 writeData(const char*, qint64 size) override { return size; }
private:
    std::shared_ptr<CleanupGate> m_gate;
};

namespace AetherSDR {
class SessionControlTestAccess {
public:
    static void terminalDisconnect(RadioModel& model)
    {
        model.m_intentionalDisconnect = true;
        model.closeConnectionForTerminalDisconnect();
    }
};
class RadioConnectionSessionTestAccess {
public:
    static void installCleanupSink(RadioConnection& wire, QStringList& commands, QList<quint32>& sequences,
                                   std::shared_ptr<CleanupGate> gate = {})
    {
        wire.m_socket->deleteLater(); // unbound wrapper, on its owner thread
        wire.m_socket = new CleanupMemorySocket(&wire, std::move(gate));
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
    void forcedCleanupDoesNotWaitOnOwner_data()
    {
        QTest::addColumn<int>("path");
        QTest::newRow("forced") << 0;
        QTest::newRow("terminal") << 1;
        QTest::newRow("multi-flex-cancel") << 2;
    }

    void forcedCleanupDoesNotWaitOnOwner()
    {
        QFETCH(int, path);
        QStringList commands;
        QList<quint32> sequences;
        const auto gate = std::make_shared<CleanupGate>();
        RadioModel model;
        RadioInfo selected = demoInfo();
        selected.family = QStringLiteral("flex");
        model.connectToRadio(selected);
        QTRY_VERIFY(model.backend()->connectionSession().has_value());
        RadioConnection* wire = model.connection();
        QSignalSpy ended(model.backend(), &IRadioBackend::disconnected);
        QMetaObject::invokeMethod(wire, [&] {
            RadioConnectionSessionTestAccess::installCleanupSink(*wire, commands, sequences, gate);
        }, Qt::BlockingQueuedConnection);
        if (path == 0) { model.forceDisconnect(); }
        else if (path == 1) { SessionControlTestAccess::terminalDisconnect(model); }
        else { model.cancelMultiFlexConflict(); }
        const bool returnedBeforeClose = !gate->finished;
        const bool retired = !model.isConnected() && !model.backend()->connectionSession();
        gate->released = true;
        QTRY_VERIFY(gate->finished);
        model.disconnectFromRadio();
        QVERIFY(returnedBeforeClose);
        QVERIFY(retired);
        QCOMPARE(ended.size(), 1);
    }

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

    void reconnectWaitsForEntireCleanup_data()
    {
        QTest::addColumn<bool>("cancelPending");
        QTest::newRow("replacement") << false;
        QTest::newRow("cancel-then-replace") << true;
    }

    void reconnectWaitsForEntireCleanup()
    {
        QFETCH(bool, cancelPending);
        QStringList commands;
        QList<quint32> sequences;
        const auto gate = std::make_shared<CleanupGate>();
        FlexBackend backend;
        QSignalSpy connected(&backend, &IRadioBackend::connected);
        QSignalSpy ended(&backend, &IRadioBackend::disconnected);
        backend.connectRadio(demoRequest());
        QTRY_COMPARE(connected.size(), 1);
        const quint64 firstEpoch = backend.connectionSession()->epoch;
        RadioConnection* wire = backend.connection();
        QMetaObject::invokeMethod(wire, [&] {
            RadioConnectionSessionTestAccess::installCleanupSink(*wire, commands, sequences, gate);
        }, Qt::BlockingQueuedConnection);
        const RadioDisconnectRequest deferred{0, RadioDisconnectRequest::Completion::Deferred};
        backend.disconnectRadio(deferred);
        QTRY_VERIFY(gate->entered);
        backend.connectRadio(demoRequest());
        if (cancelPending) {
            backend.disconnectRadio(deferred);
            backend.connectRadio(demoRequest());
        }
        // This executes INSIDE the old close's nested event loop. A plain
        // worker-queue ordering fence would already have admitted the new dial.
        quint64 transportEpoch = 0;
        QMetaObject::invokeMethod(wire, [&] { transportEpoch = wire->lineEpoch(); },
                                  Qt::BlockingQueuedConnection);
        const bool retired = !backend.isConnected() && !backend.connectionSession();
        gate->released = true;
        QTRY_COMPARE(connected.size(), 2);
        QVERIFY(gate->finished);
        QVERIFY(retired);
        QCOMPARE(transportEpoch, firstEpoch);
        QVERIFY(backend.isConnected());
        QVERIFY(backend.connectionSession()->epoch != firstEpoch);
        QCOMPARE(ended.size(), cancelPending ? 2 : 1);
        backend.disconnectRadio();
    }

    void blockingDrainAfterDeferredCleanup_data()
    {
        QTest::addColumn<int>("path");
        QTest::newRow("idle-disconnect") << 0;
        QTest::newRow("pending-disconnect") << 1;
        QTest::newRow("destruction") << 2;
    }

    void blockingDrainAfterDeferredCleanup()
    {
        QFETCH(int, path);
        QStringList commands;
        QList<quint32> sequences;
        const auto gate = std::make_shared<CleanupGate>();
        auto backend = std::make_unique<FlexBackend>();
        QSignalSpy connected(backend.get(), &IRadioBackend::connected);
        backend->connectRadio(demoRequest());
        QTRY_COMPARE(connected.size(), 1);
        RadioConnection* wire = backend->connection();
        QMetaObject::invokeMethod(wire, [&] {
            RadioConnectionSessionTestAccess::installCleanupSink(*wire, commands, sequences, gate);
        }, Qt::BlockingQueuedConnection);
        backend->disconnectRadio({0, RadioDisconnectRequest::Completion::Deferred});
        QTRY_VERIFY(gate->entered);
        if (path == 1) { backend->connectRadio(demoRequest()); }
        QMetaObject::invokeMethod(wire, [wire, gate] {
            QTimer::singleShot(25, wire, [gate] { gate->released = true; });
        }, Qt::BlockingQueuedConnection);
        if (path == 2) { backend.reset(); }
        else { backend->disconnectRadio(); }
        QVERIFY(gate->finished);
        QCOMPARE(connected.size(), 1);
        if (backend) {
            drain(wire);
            QVERIFY(!backend->isConnected());
            QVERIFY(!backend->connectionSession());
            backend->connectRadio(demoRequest());
            QTRY_COMPARE(connected.size(), 2);
        }
    }

    void gracefulCleanupUsesSharedSequenceAndOriginalStream()
    {
        QStringList commands;
        QList<quint32> sequences;
        FlexBackend backend;
        quint32 nextSequence = 700;
        backend.setIndependentTxSequenceProvider([&] { return nextSequence++; });
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
        FlexBackend backend;
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
        RadioModel model;
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
