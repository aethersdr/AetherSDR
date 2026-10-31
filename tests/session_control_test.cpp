// Inject production command sinks and socket state; no descriptors or peers.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/FlexSessionControls.h"
#include "core/backends/flex/WanConnection.h"
#include "core/backends/sim/SimBackend.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>
#include <memory>
#include <vector>

using namespace AetherSDR;
namespace AetherSDR {
class SessionControlTestAccess {
public:
    static void begin(RadioModel& model, FlexBackend& backend)
    {
        model.m_connection = backend.connection();
        model.m_panStream = backend.panStream();
        model.m_connection->m_state.store(ConnectionState::Connected);
        model.m_connection->m_handle.store(7);
        model.m_sessionCommands = SessionCommandGuard::create();
        model.m_sessionWire = model.m_connection;
        model.m_sessionWireGeneration = model.m_connection->commandSessionGeneration();
    }
    static ReceiveDispatch request(RadioModel& model, const SessionControlRequest& request,
                                   const SessionControlCompletion& completion = {})
    { return model.requestSessionControl(request, completion); }
    static void retire(RadioModel& model) { model.resetSessionControls(); }
    static void registerClient(RadioModel& model, const QString& id) { model.registerAsGuiClient(id); }
    static void setup(RadioModel& model, const RadioSetupRequest& request) { model.requestStartupSetup(request); }
    static void disconnectClients(RadioModel& model, const QList<quint32>& handles, std::function<void()> done)
    { model.disconnectClientHandlesThen(handles, std::move(done)); }
    static int pending(const RadioModel& model) { return model.m_sessionCancellations.size(); }
    static bool expire(RadioModel& model)
    {
        for (QTimer* timer : model.findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (timer->isActive() && timer->isSingleShot() && timer->interval() == 30000) {
                return QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
            }
        }
        return false;
    }
    static bool pingActive(const RadioModel& model) { return model.m_pingTimer.isActive(); }
    static void startMonitor(RadioModel& model) { model.startNetworkMonitor(); }
    static void changeTransport(RadioModel& model) { ++model.m_connection->m_sessionGeneration; }
    static bool registered(const RadioModel& model)
    { return model.m_guiClientRegistrationState.phase() == GuiClientRegistrationState::Phase::Registered; }
    static void installWriter(RadioConnection& connection, std::function<void(quint32, const QString&)> writer)
    {
        connection.m_state.store(ConnectionState::Connected);
        connection.m_commandSinkForTest = std::move(writer);
    }
    static void replaceTransport(RadioConnection& connection) { ++connection.m_sessionGeneration; }
    static void prepareWan(WanConnection& connection) { connection.m_connected = true; }
    static void replaceWan(WanConnection& connection) { ++connection.m_sessionGeneration; }
};
}

class RecordingSessionBackend final : public FlexBackend {
public:
    struct Command { QString text; SessionCommandGuard guard; RadioControlCompletion reply; };
    std::vector<Command> commands;
    QStringList setupCommands;
    std::function<void(const QString&)> onCommand;
    bool accept{true};
    bool isConnected() const override { return true; }
    RecordingSessionBackend()
    {
        setSessionCommandSink([this](const QString& text, const SessionCommandGuard& guard,
                                     const RadioControlCompletion& reply) {
            commands.push_back({text, guard, reply});
            if (onCommand) { onCommand(text); }
            return accept;
        });
        setSetupCommandSink([this](const QString& text, const RadioSetupCompletion&) {
            setupCommands.append(text);
            return true;
        });
    }
};

class SessionControlTest : public QObject {
    Q_OBJECT
    static RecordingSessionBackend* install(RadioModel& model)
    {
        auto backend = std::make_unique<RecordingSessionBackend>();
        RecordingSessionBackend* result = backend.get();
        model.setBackendForTest(std::move(backend), QStringLiteral("flex"));
        SessionControlTestAccess::begin(model, *result);
        return result;
    }
private slots:
    void encodingAndInputBounds()
    {
        using Field = SessionControlField;
        struct Case { SessionControlRequest request; QString expected; };
        const std::vector<Case> cases{
            {{Field::IdentifyProgram}, "client program AetherSDR"},
            {{Field::LowBandwidth}, "client low_bw_connect"},
            {{Field::RegisterGui}, "client gui"},
            {{Field::RegisterGui, "test-client"}, "client gui test-client"},
            {{Field::ReducedBandwidthDax}, "client set send_reduced_bw_dax=1"},
            {SessionControlRequest::udpPort(4991), "client udpport 4991"},
            {{Field::ClientIp}, "client ip"}, {{Field::RadioInfo}, "info"},
            {{Field::MicrophoneInputs}, "mic list"}, {{Field::SliceInventory}, "slice list"},
            {{Field::Keepalive}, "keepalive enable"}, {{Field::Ping}, "ping"},
            {SessionControlRequest::disconnectClient(0xab), "client disconnect 0xab"}
        };
        for (const Case& item : cases) { QCOMPARE(FlexSessionControls::encode(item.request), item.expected); }
        const QStringList topics = QStringLiteral("slice pan tx atu amplifier meter audio gps apd client xvtr tnf memories cwx dax daxiq radio codec dvk navtex usb_cable spot waveform license").split(' ');
        QCOMPARE(topics.size(), static_cast<qsizetype>(SessionTopic::Count));
        for (int i = 0; i < topics.size(); ++i) {
            QCOMPARE(FlexSessionControls::encode(SessionControlRequest::subscription(static_cast<SessionTopic>(i))),
                     QStringLiteral("sub %1 all").arg(topics[i]));
        }
        for (const SessionControlRequest& request : std::vector<SessionControlRequest>{
            {Field::Count}, {static_cast<Field>(-1)}, {Field::UdpPort, {}, 65536},
            SessionControlRequest::udpPort(0), SessionControlRequest::disconnectClient(0),
            {Field::RegisterGui, "name\nclient disconnect"}, {Field::RegisterGui, "bad|id"},
            {Field::RegisterGui, QString(129, 'a')}, {Field::Ping, "extra"},
            SessionControlRequest::subscription(SessionTopic::Count)}) {
            QVERIFY(FlexSessionControls::encode(request).isEmpty());
        }
    }
    void typedReplies()
    {
        using Field = SessionControlField;
        const auto decode = [](Field field, const QString& body) {
            return FlexSessionControls::decode(field, {RadioControlReply::Kind::Accepted, 0, body});
        };
        const SessionControlReply empty = decode(Field::SliceInventory, {});
        QVERIFY(empty.accepted()); QVERIFY(empty.slices.isEmpty());
        QCOMPARE(decode(Field::SliceInventory, "0 3 12").slices, QList<int>({0, 3, 12}));
        for (const QString& bad : {"0 nope", "-1", "0 0", "2147483648", "0\n1"}) {
            QVERIFY(!decode(Field::SliceInventory, bad).accepted());
        }
        QCOMPARE(decode(Field::MicrophoneInputs, "MIC,BAL,LINE,ACC").microphones,
                 QStringList({"MIC", "BAL", "LINE", "ACC"}));
        QVERIFY(!decode(Field::RadioInfo, QString(65537, 'x')).accepted());
        const SessionControlReply info = decode(Field::RadioInfo,
            "model=FLEX-6700,chassis_serial=ABC,software_ver=4.3.16,callsign=K1ABC,iaru_region=2,unknown=new,ip=bad");
        QVERIFY(info.accepted()); QVERIFY(info.info);
        QCOMPARE(*info.info->radio.model, QString("FLEX-6700"));
        QCOMPARE(*info.info->chassisSerial, QString("ABC"));
        QCOMPARE(*info.info->radio.iaruRegion, 2);
        QVERIFY(!info.info->radio.ip); QVERIFY(!info.info->radio.nickname);
        const SessionControlReply refused = FlexSessionControls::decode(Field::RegisterGui,
            {RadioControlReply::Kind::Refused, 42, "all slots occupied"});
        QVERIFY(!refused.accepted()); QCOMPARE(refused.result.message, QString("all slots occupied"));
    }
    void terminalWriterRejectsReplacedTransport()
    {
        RadioConnection connection;
        int writes = 0;
        SessionControlTestAccess::installWriter(connection, [&](quint32, const QString&) { ++writes; });
        const SessionCommandGuard session = SessionCommandGuard::create();
        const SessionCommandGuard request = session.request();
        const quint64 generation = connection.commandSessionGeneration();
        QVERIFY(connection.writeSessionCommand(1, "ping", generation, request));
        QCOMPARE(writes, 1);
        SessionControlTestAccess::replaceTransport(connection);
        QVERIFY(!connection.writeSessionCommand(2, "client disconnect 0x8", generation, request));
        QCOMPARE(writes, 1);
        request.cancel();
        QVERIFY(!connection.writeSessionCommand(3, "ping", connection.commandSessionGeneration(), request));
        const SessionCommandGuard another = session.request();
        session.retireSession();
        QVERIFY(!another.current());
        QVERIFY(!connection.writeSessionCommand(4, "ping", connection.commandSessionGeneration(), another));
        QCOMPARE(writes, 1);
    }
    void wanRejectsCancelledAndReplacedSession()
    {
        WanConnection connection;
        SessionControlTestAccess::prepareWan(connection);
        const quint64 generation = connection.commandSessionGeneration();
        const SessionCommandGuard guard = SessionCommandGuard::create();
        SessionControlTestAccess::replaceWan(connection);
        QCOMPARE(connection.sendSessionCommand("client disconnect 0x8", generation, guard), quint32(0));
        guard.retireSession();
        QCOMPARE(connection.sendSessionCommand("ping", connection.commandSessionGeneration(), guard), quint32(0));
    }
    void repliesCompleteOnceAndCannotCrossReconnect()
    {
        RadioModel model;
        RecordingSessionBackend* backend = install(model);
        int replies = 0;
        RadioControlReply::Kind kind = RadioControlReply::Kind::Accepted;
        SessionControlTestAccess::request(model, {SessionControlField::RadioInfo}, [&](const SessionControlReply& reply) {
            ++replies; kind = reply.result.kind;
        });
        const auto reply = backend->commands.back().reply;
        SessionControlTestAccess::changeTransport(model);
        reply({RadioControlReply::Kind::Accepted, 0, "model=FLEX-6700"});
        QCOMPARE(replies, 1); QCOMPARE(kind, RadioControlReply::Kind::Cancelled);
        reply({RadioControlReply::Kind::Accepted, 0, "model=FLEX-6700"});
        QCOMPARE(replies, 1); QCOMPARE(SessionControlTestAccess::pending(model), 0);
        SessionControlTestAccess::begin(model, *backend);
        SessionControlTestAccess::request(model, {SessionControlField::RadioInfo}, [&](const SessionControlReply& reply) {
            ++replies; kind = reply.result.kind;
        });
        const SessionCommandGuard outstanding = backend->commands.back().guard;
        SessionControlTestAccess::retire(model);
        QCOMPARE(replies, 2); QCOMPARE(kind, RadioControlReply::Kind::Cancelled);
        QVERIFY(!outstanding.current());
    }
    void disconnectSequenceAndRetirement()
    {
        RadioModel model;
        RecordingSessionBackend* backend = install(model);
        int completed = 0;
        SessionControlTestAccess::disconnectClients(model, {0, 7, 8, 8, 9}, [&] { ++completed; });
        QCOMPARE(backend->commands.size(), size_t(1));
        QCOMPARE(backend->commands[0].text, QString("client disconnect 0x8"));
        const auto first = backend->commands[0].reply;
        first({RadioControlReply::Kind::Accepted, 0, {}});
        QCOMPARE(backend->commands.size(), size_t(2));
        QCOMPARE(backend->commands[1].text, QString("client disconnect 0x9"));
        const auto second = backend->commands[1].reply;
        second({RadioControlReply::Kind::Accepted, 0, {}});
        SessionControlTestAccess::retire(model);
        SessionControlTestAccess::begin(model, *backend);
        QTest::qWait(300);
        QCOMPARE(completed, 0);
        QCOMPARE(backend->commands.size(), size_t(2));
    }
    void unansweredRequestCompletesOnce()
    {
        RadioModel model;
        RecordingSessionBackend* backend = install(model);
        int replies = 0;
        SessionControlTestAccess::request(model, {SessionControlField::SliceInventory}, [&](const SessionControlReply& reply) {
            ++replies;
            QCOMPARE(reply.result.kind, RadioControlReply::Kind::Failed);
            QVERIFY(!reply.accepted());
        });
        const auto lateReply = backend->commands.back().reply;
        const SessionCommandGuard guard = backend->commands.back().guard;
        QVERIFY(SessionControlTestAccess::expire(model));
        QCOMPARE(replies, 1);
        QCOMPARE(SessionControlTestAccess::pending(model), 0);
        QVERIFY(!guard.current());
        lateReply({RadioControlReply::Kind::Accepted, 0, {}});
        QCOMPARE(replies, 1);
    }
    void failedDisconnectCannotStrandInitialization()
    {
        RadioModel model;
        RecordingSessionBackend* backend = install(model);
        QSignalSpy errors(&model, &RadioModel::connectionError);
        int continued = 0;
        SessionControlTestAccess::disconnectClients(model, {8, 9}, [&] { ++continued; });
        const auto lateReply = backend->commands.back().reply;
        QVERIFY(SessionControlTestAccess::expire(model));
        QCOMPARE(errors.size(), 1);
        QCOMPARE(continued, 0);
        lateReply({RadioControlReply::Kind::Accepted, 0, {}});
        QCOMPARE(backend->commands.size(), size_t(1));
    }
    void reentrantMonitorCannotStartOldTimer()
    {
        RadioModel model;
        install(model);
        connect(&model, &RadioModel::adaptiveThrottleChanged, &model,
                [&] { SessionControlTestAccess::retire(model); });
        SessionControlTestAccess::startMonitor(model);
        QVERIFY(!SessionControlTestAccess::pingActive(model));
    }
    void registrationOrderAndSubscriptionBatch()
    {
        RadioModel model;
        RecordingSessionBackend* backend = install(model);
        AppSettings::instance().setValue("LowBandwidthConnect", "True");
        QSignalSpy enumeration(&model, &RadioModel::sliceConnectEnumerationStarted);
        SessionControlTestAccess::registerClient(model, "test-client");
        QCOMPARE(backend->commands.size(), size_t(3));
        QCOMPARE(backend->commands[0].text, QString("client program AetherSDR"));
        QCOMPARE(backend->commands[1].text, QString("client low_bw_connect"));
        QCOMPARE(backend->commands[2].text, QString("client gui test-client"));
        QCOMPARE(enumeration.size(), 1);
        // Stop before datagram setup; no test opens a radio transport.
        backend->onCommand = [&](const QString& text) {
            if (text == "mic list") { SessionControlTestAccess::retire(model); }
        };
        const auto registered = backend->commands[2].reply;
        registered({RadioControlReply::Kind::Accepted, 0, {}});
        QVERIFY(SessionControlTestAccess::registered(model));
        QStringList subscriptions;
        for (const auto& command : backend->commands) {
            if (command.text.startsWith("sub ")) {
                subscriptions.append(command.text);
                QVERIFY(!command.reply); // no per-topic acknowledgement wait
            }
        }
        QCOMPARE(subscriptions, QStringList({"sub slice all", "sub pan all", "sub tx all", "sub atu all",
            "sub amplifier all", "sub meter all", "sub audio all", "sub gps all", "sub apd all",
            "sub client all", "sub xvtr all"}));
        AppSettings::instance().setValue("LowBandwidthConnect", "False");
    }
    void startupDoesNotPersistSettings()
    {
        RadioModel model;
        RecordingSessionBackend* backend = install(model);
        AppSettings& settings = AppSettings::instance();
        settings.setValue("StationName", "Saved Station");
        const QString clientId = settings.effectiveGuiClientId();
        SessionControlTestAccess::setup(model, {RadioSetupField::ClientStation, QString("Transient Station")});
        SessionControlTestAccess::setup(model, {RadioSetupField::NetworkMtu, 1200});
        QCOMPARE(settings.value("StationName").toString(), QString("Saved Station"));
        QCOMPARE(settings.effectiveGuiClientId(), clientId);
        QCOMPARE(backend->setupCommands, QStringList({QString("client station Transient") + QChar(0x7f) + "Station",
            "client set enforce_network_mtu=1 network_mtu=1200"}));
    }
    void reentrantEnumerationStopsRegistration()
    {
        RadioModel model;
        RecordingSessionBackend* backend = install(model);
        connect(&model, &RadioModel::sliceConnectEnumerationStarted, &model,
                [&] { SessionControlTestAccess::retire(model); });
        SessionControlTestAccess::registerClient(model, "test-client");
        QCOMPARE(backend->commands.size(), size_t(1));
        QCOMPARE(backend->commands[0].text, QString("client program AetherSDR"));
    }
    void unsupportedBackendDoesNotBootstrap()
    {
        SimBackend backend;
        QVERIFY(!backend.capabilities().sessionControl);
        QCOMPARE(backend.requestSessionControl({SessionControlField::RegisterGui}, SessionCommandGuard::create()),
                 ReceiveDispatch::Unsupported);
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("session-controls"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    SessionControlTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "session_control_test.moc"
