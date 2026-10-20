// Socket-free production bridge/model/backend/audio paths. Inject decoded
// statuses and command replies; no firmware peer, device, listener or RF.
#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"
#include "core/AudioEngine.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/RadioConnection.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TxController.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QFile>
#include <QDateTime>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <vector>
#include <limits>

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject call(AutomationServer& bridge, const QJsonObject& request)
    { return bridge.handleLine(QJsonDocument(request).toJson(QJsonDocument::Compact), nullptr); }
};
class TxOperationIntegrationTestAccess {
public:
    static void identity(RadioConnection& connection, bool connected)
    {
        connection.m_handle.store(0x12345678);
        connection.m_state.store(connected ? ConnectionState::Connected : ConnectionState::Disconnected);
    }
};
class DaxTxOwnershipTestAccess {
public:
    static void timeout(RadioModel& model)
    { if (model.m_daxTxOwnershipIntent) { model.expireDaxTxOwnership(model.m_daxTxOwnershipIntent->ticket); } }
    static void disconnect(RadioModel& model)
    { model.m_intentionalDisconnect = true; model.onDisconnected(); }
    static RadioModel::ResponseCallback takeCreateReply(RadioModel& model)
    {
        if (model.m_pendingCallbacks.isEmpty()) { return {}; }
        const quint32 seq = model.m_pendingCallbacks.cbegin().key();
        return model.m_pendingCallbacks.take(seq);
    }
    static void holdProfile(RadioModel& model, int milliseconds = 1000)
    {
        model.m_profileLoadRadioStateWriteHoldUntilMs = QDateTime::currentMSecsSinceEpoch() + milliseconds;
        model.refreshDaxTxOwnershipGuards();
    }
    static void reopenTransmit(RadioModel& model) { model.m_txSessionClosing = false; }
};
class AudioEngineRatesTestAccess {
public:
    static void remote(AudioEngine& audio, const TxCoordinator::Context& context)
    { audio.sendVoiceTxPacket(QByteArray(128 * 2 * sizeof(qint16), '\0'), 0x4b000001, context); }
};
}

using namespace AetherSDR;
namespace {
constexpr quint32 kStream = 0x4a000001;
int failures = 0;
void check(bool passed, const char* claim)
{
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", claim);
    failures += !passed;
}
struct Write {
    QString command;
    FlexBackend::CommandReply reply;
    DaxTxRequestGuard guard;
};
struct Fixture {
    RadioModel model;
    AudioEngine audio;
    AutomationServer bridge;
    FlexBackend* backend{nullptr};
    std::vector<Write> writes;
    std::vector<DaxTxAudioPermit> packets;

    Fixture()
    {
        model.rebuildBackendForTest(QStringLiteral("flex"));
        backend = static_cast<FlexBackend*>(model.backend());
        RadioConnection* connection = backend->connection();
        QMetaObject::invokeMethod(connection, [connection] {
            TxOperationIntegrationTestAccess::identity(*connection, true);
        }, Qt::BlockingQueuedConnection);
        backend->setDaxTxCommandSink([this](const QString& command, FlexBackend::CommandReply reply,
                                           const DaxTxRequestGuard& guard) {
            writes.push_back({command, std::move(reply), guard});
            return true;
        });
        QObject::connect(&model, &RadioModel::daxTxAudioRouteChanged,
                         &audio, &AudioEngine::setDaxTxAudioRoute);
        QObject::connect(&audio, &AudioEngine::txPacketReady, &audio,
            [this](const QByteArray&, const TxCoordinator::Context&, const DaxTxAudioPermit& permit) {
                packets.push_back(permit);
            });
        model.handleSliceStatusForTest(0, {{QStringLiteral("in_use"), QStringLiteral("1")},
            {QStringLiteral("client_handle"), QStringLiteral("0x12345678")},
            {QStringLiteral("tx"), QStringLiteral("1")}, {QStringLiteral("mode"), QStringLiteral("USB")},
            {QStringLiteral("RF_frequency"), QStringLiteral("14.200000")}});
        bridge.setRadioModel(&model);
        bridge.setAudioEngine(&audio);
    }
    ~Fixture()
    {
        // Queued setters and destructor callbacks must not reference test vectors
        // after those vectors have been destroyed.
        QObject::disconnect(&model, nullptr, &audio, nullptr);
        QObject::disconnect(&audio, nullptr, &audio, nullptr);
    }
    QJsonObject snapshot() { return model.daxTxOwnershipSnapshot(); }
    QJsonObject action(const QString& action)
    { return AutomationServerTestAccess::call(bridge, {{"cmd", "daxTx"}, {"action", action}}); }
    void stream(bool tx, const QString& owner = QStringLiteral("0x12345678"),
                quint32 id = kStream, bool legacy = false)
    {
        QMap<QString, QString> fields{{QStringLiteral("client_handle"), owner},
                                    {QStringLiteral("tx"), tx ? QStringLiteral("1") : QStringLiteral("0")}};
        if (!legacy) { fields.insert(QStringLiteral("type"), QStringLiteral("dax_tx")); }
        model.handleStatusForTest(QStringLiteral("%1 0x%2")
            .arg(legacy ? QStringLiteral("tx_audio_stream") : QStringLiteral("stream"))
            .arg(id, 0, 16), fields);
    }
    void partial(const QMap<QString, QString>& fields)
    { model.handleStatusForTest(QStringLiteral("stream 0x4a000001"), fields); }
    void removed()
    { model.handleStatusForTest(QStringLiteral("stream 0x4a000001 removed"), {}); }
    void reply(size_t index, int code = 0)
    {
        const FlexBackend::CommandReply callback = writes.at(index).reply;
        callback(code, code ? QStringLiteral("refused") : QString());
    }
};

void ordering(bool statusFirst)
{
    Fixture f;
    f.stream(false);
    const QJsonObject admitted = f.action(QStringLiteral("take"));
    std::printf("[BRIDGE synthetic take] %s\n", QJsonDocument(admitted).toJson(QJsonDocument::Compact).constData());
    check(admitted.value("ok").toBool() && f.writes.size() == 1
          && f.writes[0].command == QLatin1String("stream set 0x4a000001 tx=1"),
          "bridge take dispatches only the exact client stream-selection command");
    check(!f.snapshot().value("owned").toBool(), "dispatch is not observed ownership");
    check(f.action(QStringLiteral("take")).value("ok").toBool() && f.writes.size() == 1,
          "repeated pending take is idempotent");
    check(!f.action(QStringLiteral("release")).value("ok").toBool() && f.writes.size() == 1,
          "an opposite pending operation is refused instead of racing");
    if (statusFirst) { f.stream(true); } else { f.reply(0); }
    check(f.snapshot().value("pending").toBool(), "one half of the ACK/status pair cannot finish the request");
    if (statusFirst) { f.reply(0); } else { f.stream(true); }
    std::printf("[BRIDGE synthetic confirmation] %s\n", QJsonDocument(f.action(QStringLiteral("status"))).toJson(QJsonDocument::Compact).constData());
    check(f.model.daxTxStreamReady() && !f.snapshot().value("pending").toBool(),
          "both ACK/status orderings converge to radio-confirmed ownership");
    check(f.action(QStringLiteral("take")).value("ok").toBool() && f.writes.size() == 1,
          "take on confirmed ownership writes nothing");
    check(f.action(QStringLiteral("release")).value("ok").toBool()
          && f.writes.back().command == QLatin1String("stream set 0x4a000001 tx=0")
          && !f.model.daxTxStreamReady(), "release immediately closes DAX media admission");
    f.stream(false, QStringLiteral("0x12345678"), kStream, true);
    f.reply(1);
    check(!f.snapshot().value("owned").toBool() && !f.snapshot().value("pending").toBool(),
          "legacy status and ACK confirm release without changing global DAX or TX");
    check(!f.model.isRadioTransmitting() && f.model.txClientHandle() == 0,
          "ownership actions do not key or grant transmitter-client ownership");
}

void failuresAndClients()
{
    Fixture f;
    f.stream(false);
    f.stream(true, QStringLiteral("0x87654321"), 0x4a000002);
    check(f.snapshot().value("streamId").toString() == QLatin1String("0x4a000001")
          && !f.snapshot().value("owned").toBool(), "a second client's stream cannot replace the local stream");
    f.action(QStringLiteral("take"));
    f.reply(0, 0x50000001);
    check(!f.snapshot().value("pending").toBool() && !f.model.daxTxStreamReady()
          && !f.snapshot().value("error").toString().isEmpty(), "radio refusal is visible and does not fabricate ownership");
    f.action(QStringLiteral("take"));
    const DaxTxRequestGuard expired = f.writes.back().guard;
    DaxTxOwnershipTestAccess::timeout(f.model);
    check(!expired.permits() && f.snapshot().value("state").toString() == QLatin1String("timed-out"),
          "timeout revokes the exact queued command rather than replaying it");
    const size_t before = f.writes.size();
    check(!f.action(QStringLiteral("take")).value("ok").toBool() && f.writes.size() == before,
          "an ambiguous timeout cannot be retried without fresh status");
    f.reply(1);
    check(!f.model.daxTxStreamReady(), "a late success ACK cannot turn timeout into ownership");
    f.stream(false);
    check(f.action(QStringLiteral("take")).value("ok").toBool() && f.writes.size() == before + 1,
          "fresh radio state permits a new explicit request");
    const DaxTxRequestGuard retired = f.writes.back().guard;
    f.stream(true, QStringLiteral("0x87654321"));
    check(!retired.permits() && !f.model.daxTxStreamReady(), "same-ID foreign ownership revokes both command and media");
    f.reply(2);
    check(!f.snapshot().value("owned").toBool(), "a stale ACK cannot reclaim a foreign reused ID");
    f.partial({{QStringLiteral("tx"), QStringLiteral("1")}});
    check(!f.model.daxTxStreamReady(), "ownerless old status cannot resurrect a retired stream");
    f.stream(false);
    f.action(QStringLiteral("take"));
    f.stream(true);
    f.stream(false);
    f.reply(3);
    check(!f.model.daxTxStreamReady() && f.snapshot().value("pending").toBool(),
          "a newer contrary status defeats an older matching echo even with a late ACK");
    DaxTxOwnershipTestAccess::timeout(f.model);
}

void mediaAndSourceFlags()
{
    Fixture f;
    TxCoordinator coordinator([](const auto&, auto) {});
    const TxCoordinator::Actor actor = coordinator.registerActor({true, 0});
    const TxCoordinator::Producer producer = coordinator.registerProducer();
    const TxCoordinator::Operation operation = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    const TxCoordinator::Context context = coordinator.mediaContext(producer, operation);
    f.audio.setTransmitting(true);  // packetizer state only; no model key request
    const QByteArray pcm(128 * 2 * sizeof(float), '\0');
    f.stream(false);
    f.audio.sendModemTxAudio(pcm, context);
    check(f.packets.empty(), "modem producers cannot send on a released stream");
    f.action(QStringLiteral("take")); f.reply(0); f.stream(true);
    f.audio.sendModemTxAudio(pcm, context);
    check(f.packets.size() == 1 && f.packets.front().permits(), "confirmed DAX ownership admits original producer media");
    const DaxTxAudioPermit queued = f.packets.front();
    f.action(QStringLiteral("release"));
    check(!queued.permits(), "release revokes an already queued packet at the final-writer permit");
    f.reply(1); f.stream(false);
    f.action(QStringLiteral("take")); f.reply(2); f.stream(true);
    check(!queued.permits(), "a new take never renews an old packet's incarnation");
    f.removed();
    f.audio.sendModemTxAudio(pcm, context);
    check(f.packets.size() == 1, "stream removal closes the production modem packetizer");
    AudioEngineRatesTestAccess::remote(f.audio, context);
    check(f.packets.size() == 2 && f.packets.back().permits(), "remote PC-microphone packets retain their independent route");
    f.audio.setHostModulation(true);
    int seamFrames = 0;
    QObject::connect(&f.audio, &AudioEngine::txTransportPcmReady, &f.audio,
        [&](const QByteArray&, TxAudioSource, const TxCoordinator::Context&) { ++seamFrames; });
    f.audio.sendModemTxAudio(pcm, context);
    check(seamFrames == 1, "seam-audio backends do not acquire a Flex DAX dependency");
    for (const bool sourceOn : {false, true}) {
        TransmitDelta delta; delta.dax = sourceOn;
        f.model.transmitModel().applyChanges(delta);
        f.stream(false);
        const size_t index = f.writes.size();
        f.action(QStringLiteral("take")); f.reply(index); f.stream(true);
        f.action(QStringLiteral("release")); f.reply(index + 1); f.stream(false);
        check(f.model.transmitModel().daxOn() == sourceOn,
              "stream handoff preserves either radio-global DAX source value");
    }
    for (const Write& write : f.writes) {
        check(write.command.startsWith(QLatin1String("stream set ")),
              "ownership transport contains no source-flag, TX-slice or keying writes");
    }
    check(!f.model.isRadioTransmitting(), "synthetic bridge ownership proof initiates no model transmission");
}

void lifecycle()
{
    Fixture f;
    f.stream(false);
    f.action(QStringLiteral("take"));
    const DaxTxRequestGuard old = f.writes[0].guard;
    f.removed();
    f.stream(true);
    f.reply(0);
    check(!old.permits() && !f.snapshot().value("pending").toBool(),
          "removal retires command identity even when the numeric ID is reused");
    f.stream(false);
    f.action(QStringLiteral("take"));
    RadioConnection* connection = f.backend->connection();
    QMetaObject::invokeMethod(connection, [connection] {
        TxOperationIntegrationTestAccess::identity(*connection, false);
    }, Qt::BlockingQueuedConnection);
    DaxTxOwnershipTestAccess::disconnect(f.model);
    f.reply(1);
    check(!f.model.daxTxStreamReady() && f.snapshot().value("streamId").toString() == QLatin1String("0x00000000"),
          "disconnect retires ownership before pending replies are drained");
    QMetaObject::invokeMethod(connection, [connection] {
        TxOperationIntegrationTestAccess::identity(*connection, true);
    }, Qt::BlockingQueuedConnection);
    check(!f.model.daxTxStreamReady() && f.writes.size() == 2,
          "reconnect does not replay remembered take intent");
    f.stream(false);
    f.action(QStringLiteral("take"));
    const FlexBackend::CommandReply replaced = f.writes.back().reply;
    f.model.rebuildBackendForTest(QStringLiteral("sim"));
    replaced(0, {});
    check(!f.model.daxTxStreamReady() && f.snapshot().value("streamId").toString() == QLatin1String("0x00000000"),
          "production backend replacement retires ownership independently of disconnect");
}

void exactInputAndKeyFences()
{
    Fixture f;
    f.stream(true);
    const std::shared_ptr<TxController> controller = f.model.localTxController();
    check(controller && controller->valid() && f.snapshot().value("canRelease").toBool(),
          "an idle local controller lifetime sentinel does not block handoff");
    const TxCoordinator::Producer producer = f.model.registerTxProducer(&f.audio);
    {
        const TxCoordinator::Request abandoned = producer.request();
        f.model.noteDaxTxInput(abandoned);
        check(!f.snapshot().value("canRelease").toBool(), "an explicit original DAX preparation blocks disruptive handoff");
    }
    check(f.snapshot().value("canRelease").toBool(), "abandoned original inputs are weak and cannot strand busy state");
    const TxCoordinator::Request root = producer.request();
    const TxCoordinator::Request child = root.derive();
    f.model.noteDaxTxInput(root);
    std::vector<TxCoordinator::Command> keys;
    f.backend->setTxCommandSink([&](const QString&, const TxCoordinator::Command& command) {
        keys.push_back(command);
    });
    const bool keyed = f.model.setProducerTransmit(child, true, TransmitModel::PttSource::Dax);
    check(keyed && !keys.empty() && keys.front().permitsDispatch(TxCoordinator::monotonicMs()),
          "injected DAX key command retains existing TX admission before ownership loss");
    if (!keys.empty()) {
        const TxCoordinator::Command queuedKey = keys.front();
        f.removed();
        check(!root.valid() && !child.valid() && !queuedKey.permitsDispatch(TxCoordinator::monotonicMs()),
              "removal fences original, derived preparation and already queued DAX key command");
        f.stream(true);
        check(!queuedKey.permitsDispatch(TxCoordinator::monotonicMs()) && !root.valid(),
              "same-ID ownership reacquisition never renews a queued key or original program");
        check(!f.model.isRadioTransmitting(), "exact DAX cleanup ends its local optimistic transmission");
    }
    f.backend->setTxCommandSink({});
    keys.clear();

    Fixture direct;
    direct.stream(true);
    const TxCoordinator::Producer directProducer = direct.model.registerTxProducer(&direct.audio);
    const TxCoordinator::Request directInput = directProducer.request();
    direct.model.noteDaxTxInput(directInput);
    std::vector<TxCoordinator::Command> directKeys;
    direct.backend->setTxCommandSink([&](const QString&, const TxCoordinator::Command& command) {
        directKeys.push_back(command);
    });
    check(direct.model.setProducerTransmit(directInput, true, TransmitModel::PttSource::Dax),
          "a directly admitted original DAX request uses the production keying path");
    direct.stream(false);
    check(!directInput.valid() && !directKeys.empty()
          && !directKeys.front().permitsDispatch(TxCoordinator::monotonicMs()),
          "ownership loss denies a directly admitted queued key without changing generic close semantics");
    direct.backend->setTxCommandSink({});
    directKeys.clear();
}

void cancellationAndMalformedStatus()
{
    Fixture wspr;
    wspr.stream(false);
    const TxCoordinator::Producer producer = wspr.model.registerTxProducer(&wspr.audio);
    const TxCoordinator::Request original = producer.request();
    wspr.model.noteDaxTxInput(original);
    check(wspr.model.prepareWsprTransmit(original) && wspr.writes.size() == 1,
          "WSPR prepares its original input through the same typed ownership path");
    if (!wspr.writes.empty()) {
        const DaxTxRequestGuard queued = wspr.writes.front().guard;
        wspr.model.releaseWsprTransmit(original);
        check(!queued.permits() && !wspr.model.daxTxStreamReady(),
              "WSPR cancellation revokes a take already accepted into the command queue");
        wspr.reply(0);
        wspr.stream(false);
        check(wspr.writes.size() == 1, "a canceled queued take is never blindly replayed or rolled back");
    }
    Fixture timed;
    timed.stream(false);
    const TxCoordinator::Producer timedProducer = timed.model.registerTxProducer(&timed.audio);
    const TxCoordinator::Request timedInput = timedProducer.request();
    timed.model.noteDaxTxInput(timedInput);
    check(timed.model.prepareWsprTransmit(timedInput), "WSPR ownership can wait for status without keying");
    DaxTxOwnershipTestAccess::timeout(timed.model);
    timed.stream(true);
    check(!timedInput.valid() && !timed.model.hasWsprTxStream(),
          "late ownership after timeout cannot arm the old beacon input");
    timed.model.releaseWsprTransmit(timedInput);

    Fixture malformed;
    malformed.stream(false);
    malformed.action(QStringLiteral("take"));
    const DaxTxRequestGuard queued = malformed.writes.front().guard;
    malformed.partial({{QStringLiteral("tx"), QStringLiteral("2")}});
    check(!queued.permits() && !malformed.snapshot().value("txKnown").toBool()
          && !malformed.snapshot().value("pending").toBool(),
          "malformed TX status retires both observation and pending mutation");
    malformed.reply(0);
    check(!malformed.model.daxTxStreamReady(), "late ACK cannot restore malformed ownership");
}

void createOrderingAndGuardRefresh()
{
    Fixture f;
    check(f.action(QStringLiteral("take")).value("ok").toBool(), "explicit take admits local allocation");
    const auto reply = DaxTxOwnershipTestAccess::takeCreateReply(f.model);
    if (reply) { reply(0, QStringLiteral("0x4a000001")); }
    check(reply && !f.model.daxTxStreamReady() && f.writes.empty(),
          "create ACK alone allocates no send permission or selection");
    check(f.model.ensureDaxTxStream(DaxTxRequestReason::ExplicitOwnership)
          && !DaxTxOwnershipTestAccess::takeCreateReply(f.model),
          "allocated ID awaiting owner status cannot trigger duplicate stream creation");
    f.stream(false);
    check(f.writes.size() == 1, "confirmed local create identity resumes only the current explicit take");
    if (!f.writes.empty()) { f.reply(0); f.stream(true); }

    Fixture profile;
    profile.stream(false);
    profile.action(QStringLiteral("take"));
    const DaxTxRequestGuard guard = profile.writes.front().guard;
    int refreshes = 0;
    QObject::connect(&profile.model, &RadioModel::daxTxOwnershipChanged,
                     &profile.audio, [&] { ++refreshes; });
    DaxTxOwnershipTestAccess::holdProfile(profile.model, 10);
    check(!guard.permits() && !profile.snapshot().value("canTake").toBool() && refreshes > 0,
          "profile hold interrupts queued ownership and refreshes availability immediately");
    const int before = refreshes;
    QEventLoop loop;
    QTimer::singleShot(30, &loop, &QEventLoop::quit);
    loop.exec();
    check(refreshes > before, "profile hold expiry refreshes availability without unrelated model traffic");
    QObject::disconnect(&profile.model, nullptr, &profile.audio, nullptr);

    Fixture active;
    active.stream(false);
    active.action(QStringLiteral("take"));
    const DaxTxRequestGuard activeGuard = active.writes.front().guard;
    active.model.handleStatusForTest(QStringLiteral("interlock"),
        {{QStringLiteral("state"), QStringLiteral("TRANSMITTING")},
         {QStringLiteral("tx_client_handle"), QStringLiteral("0x87654321")}});
    check(!activeGuard.permits() && !active.snapshot().value("pending").toBool(),
          "a later active transmission revokes the pending stream handoff");

    Fixture reentrant;
    reentrant.stream(false);
    bool replaced = false;
    QObject::connect(&reentrant.model, &RadioModel::daxTxOwnershipChanged, &reentrant.audio, [&] {
        if (!replaced && reentrant.snapshot().value("pending").toBool()) {
            replaced = true;
            reentrant.model.rebuildBackendForTest(QStringLiteral("sim"));
        }
    });
    check(!reentrant.action(QStringLiteral("take")).value("ok").toBool()
          && replaced && reentrant.writes.empty() && !reentrant.model.daxTxStreamReady(),
          "reentrant backend replacement during intent publication cannot dispatch the old request");
}

void creationAndBusy()
{
    Fixture f;
    f.action(QStringLiteral("take"));
    const auto create = DaxTxOwnershipTestAccess::takeCreateReply(f.model);
    check(bool(create), "explicit take can request local stream creation without selecting a foreign stream");
    f.model.handleStatusForTest(QStringLiteral("stream 0x4a000001 removed"), {});
    if (create) { create(0, QStringLiteral("0x4a000001")); }
    check(f.writes.empty() && !f.model.daxTxStreamReady(), "a create ACK after removal cannot republish or take the retired ID");
    Fixture busy;
    busy.stream(true);
    busy.model.handleStatusForTest(QStringLiteral("interlock"),
        {{QStringLiteral("state"), QStringLiteral("TRANSMITTING")},
         {QStringLiteral("tx_client_handle"), QStringLiteral("0x87654321")}});
    check(!busy.action(QStringLiteral("release")).value("ok").toBool() && busy.writes.empty(),
          "an active foreign transmission prevents disruptive handoff");
    Fixture profile;
    profile.stream(false);
    DaxTxOwnershipTestAccess::holdProfile(profile.model);
    check(!profile.action(QStringLiteral("take")).value("ok").toBool() && profile.writes.empty(),
          "profile load hold cannot trigger or admit ownership mutation");
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("dax-tx-ownership-test"));
    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication app(argc, argv);
    check(settings.isValid(), "settings are isolated");
    ordering(false); ordering(true);
    failuresAndClients(); mediaAndSourceFlags(); lifecycle(); creationAndBusy();
    exactInputAndKeyFences(); cancellationAndMalformedStatus(); createOrderingAndGuardRefresh();
    return failures ? 1 : 0;
}
