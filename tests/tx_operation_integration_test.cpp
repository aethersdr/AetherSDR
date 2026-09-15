// Socket-free production-path tests. Only an injected transport recorder is
// used; no radio, peer, listener, discovery or transmitter is opened.
#include "TestSettingsProfile.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/ClientQuindarTone.h"
#include "core/PanadapterStream.h"

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <memory>
#include <limits>

using namespace AetherSDR;

namespace AetherSDR {
class TxOperationIntegrationTestAccess {
public:
    static void transmitDelta(RadioModel& radio, const TransmitDelta& delta)
    {
        radio.applyBackendTransmitDelta(delta);
    }
    static bool cwxDrainArmed(const RadioModel& radio) { return radio.m_cwxDrainArmed; }
    static bool txSessionClosing(const RadioModel& radio) { return radio.m_txSessionClosing; }
    static void injectTcp(RadioModel& radio, RadioConnection& connection, QStringList& commands)
    {
        connection.m_commandSinkForTest = [&commands](quint32, const QString& command) { commands << command; };
        radio.m_family = QStringLiteral("flex");
        radio.m_connection = &connection;
    }
    static void teardownWithPendingReply(RadioModel& radio, RadioModel::ResponseCallback callback)
    {
        radio.m_pendingCallbacks.insert(1234, std::move(callback));
        radio.teardownBackend();
    }
    static void insertPendingReply(RadioModel& radio, quint32 sequence,
                                   RadioModel::ResponseCallback callback)
    {
        radio.m_pendingCallbacks.insert(sequence, std::move(callback));
    }
    static qsizetype pendingReplyCount(const RadioModel& radio)
    {
        return radio.m_pendingCallbacks.size();
    }
    static void disconnect(RadioModel& radio)
    {
        radio.onDisconnected();
    }
    static RadioConnection* connection(RadioModel& radio)
    {
        return radio.m_connection;
    }
    static quint32 firstPendingReplySequence(const RadioModel& radio)
    {
        return radio.m_pendingCallbacks.isEmpty() ? 0 : radio.m_pendingCallbacks.cbegin().key();
    }
    static void beginMultiFlexProbe(RadioModel& radio)
    {
        radio.peekForMultiFlexConflictThen([] {});
    }
    static bool hasMultiFlexContinuation(const RadioModel& radio)
    {
        return static_cast<bool>(radio.m_multiFlexContinuation);
    }
    static void disconnectClientsThen(RadioModel& radio, const QList<quint32>& handles,
                                      std::function<void()> continuation)
    {
        radio.disconnectClientHandlesThen(handles, std::move(continuation));
    }
    static void injectNetCwTransport(RadioModel& radio, PanadapterStream& stream,
                                    std::function<void(const QByteArray&)> sink)
    {
        // No init(), start(), sockets, or synthetic firmware. Only replace
        // the final writer and drive the real public CW methods/scheduler.
        stream.m_packetSinkForTest = std::move(sink);
        radio.m_family = QStringLiteral("flex");
        radio.m_panStream = &stream;
        radio.m_netCwStreamId = 0x12345678;
    }
};
} // namespace AetherSDR

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}

class RecordingBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{false};
    QString cwRejection;
    QStringList* commands;
    explicit RecordingBackend(QStringList& record) : commands(&record)
    {
        caps.canTransmit = true;
        caps.hasRadioSideCwKeyer = true;
        caps.hasTuner = true;
    }
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool on) override { *commands << (on ? "mox:on" : "mox:off"); }
    void setTune(bool on, int) override { *commands << (on ? "tune:on" : "tune:off"); }
    void setAtu(bool on) override { *commands << (on ? "atu:on" : "atu:off"); }
    void setCwKeying(bool on, bool, int) override { *commands << (on ? "cw:on" : "cw:off"); }
    QString sendCwText(const QString& text) override { *commands << "cwx:" + text; return cwRejection; }
    void abortCwText() override { *commands << "cwx:abort"; }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

struct Fixture {
    // Recorder outlives the RadioModel's backend and its shutdown cleanup.
    QStringList commands;
    RadioModel radio;
    RecordingBackend* backend;
    Fixture()
    {
        auto owned = std::make_unique<RecordingBackend>(commands);
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("test"));
        if (!radio.automationApplySliceFixture(0, QStringLiteral("A")) || !radio.slice(0)) {
            qFatal("Could not install the disconnected slice fixture");
        }
        SliceDelta delta;
        delta.txSlice = true;
        delta.mode = QStringLiteral("USB");
        delta.panId = QStringLiteral("0x40000000");
        radio.slice(0)->applyChanges(delta);
        backend->connected = true;
        radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });
        commands.clear();
    }
};

void primaryRoutes()
{
    Fixture f;
    TransmitModel& tx = f.radio.transmitModel();
    QStringList rawKeying;
    QObject::connect(&tx, &TransmitModel::commandReady, &f.radio, [&](const QString& command) {
        if (command.startsWith("xmit ") || command.startsWith("transmit tune ")
            || command == "atu start" || command == "atu bypass") {
            rawKeying << command;
        }
    });
    tx.setMox(true);
    const TxCoordinator::Operation first = f.radio.transmitOperation();
    check(first.permitsDispatch(std::numeric_limits<qint64>::max()) && first.permitsCleanup(),
          "MOX acquires a live engine operation without a new operator timeout");
    tx.setMox(false);
    check(!first.permitsDispatch(std::numeric_limits<qint64>::max()), "explicit MOX release fences queued key-on");
    tx.startTune();
    tx.stopTune();
    tx.startTwoToneTune();
    tx.stopTune();
    tx.atuStart();
    tx.atuBypass();
    f.radio.sendCwKey(true);
    f.radio.sendCwKey(false);
    f.radio.sendCwPtt(true);
    f.radio.sendCwPtt(false);
    check(f.commands == QStringList({"mox:on", "mox:off", "tune:on", "tune:off",
          "tune:on", "tune:off", "atu:on", "atu:off", "cw:on", "cw:off", "mox:on", "mox:off"}),
          "every primary intent dispatches once through its typed backend verb");
    check(rawKeying.isEmpty(), "no duplicate keying escapes via raw model command text");
}

void refusedStartsAndUnconditionalStops()
{
    Fixture f;
    f.backend->caps.canTransmit = false;
    TransmitModel& tx = f.radio.transmitModel();
    tx.setMox(true);
    tx.requestPttOn(TransmitModel::PttSource::Mox);
    tx.startTune();
    tx.startTwoToneTune();
    tx.atuStart();
    f.radio.setTransmit(true);
    f.radio.sendCwKey(true);
    f.radio.sendCwPaddle(true, false);
    f.radio.sendCwPtt(true);
    f.radio.sendCwKeyEdge(true);
    f.radio.cwxModel().send("CQ");
    f.radio.cwxModel().sendChar("E");
    f.radio.cwxModel().sendMacro(1);
    check(f.commands.isEmpty(), "RX-only backend refuses every primary start including CW/CWX");
    check(!tx.isTransmitting() && !tx.isTuning(), "refusal cannot leave optimistic TX or TUNE latched");
    tx.setMox(false);
    tx.stopTune();
    tx.atuBypass();
    f.radio.sendCwKey(false);
    f.radio.sendCwPtt(false);
    f.radio.cwxModel().clearBuffer();
    check(f.commands == QStringList({"mox:off", "tune:off", "atu:off", "cw:off", "mox:off", "cwx:abort"}),
          "key-up, bypass and clear remain available after capability loss");
}

void cwTuneMutualExclusion()
{
    Fixture f;
    TransmitModel& tx = f.radio.transmitModel();

    tx.startTune();
    check(f.commands == QStringList({"tune:on"}) && tx.isTuning(),
          "TUNE starts through the coordinator before the CW exclusion applies");
    f.commands.clear();
    f.radio.sendCwKey(true, QStringLiteral("test-straight-key"));
    f.radio.sendCwKeyEdge(true, QStringLiteral("test-iambic-key"));
    f.radio.cwxModel().send(QStringLiteral("CQ"));
    check(f.commands.isEmpty(),
          "active TUNE refuses straight-key, iambic and CWX key-down intent");
    f.radio.sendCwKey(false, QStringLiteral("test-straight-key"));
    f.radio.sendCwKeyEdge(false, QStringLiteral("test-iambic-key"));
    check(f.commands == QStringList({"cw:off", "cw:off"}) && tx.isTuning(),
          "CW key-up cleanup remains available without ending the TUNE operation");
    tx.stopTune();

    f.commands.clear();
    f.radio.sendCwKey(true, QStringLiteral("test-straight-key"));
    tx.startTune();
    check(f.commands == QStringList({"cw:on"}) && !tx.isTuning(),
          "an active CW key operation refuses TUNE before optimistic state or dispatch");
    f.radio.sendCwKey(false, QStringLiteral("test-straight-key"));
    tx.startTune();
    check(f.commands == QStringList({"cw:on", "cw:off", "tune:on"}) && tx.isTuning(),
          "TUNE is re-admitted after the CW key operation releases");
    tx.stopTune();

    f.commands.clear();
    f.radio.setCwPaddleHeld(true);
    tx.startTune();
    check(f.commands.isEmpty() && !tx.isTuning(),
          "a held paddle refuses TUNE during the keyer's inter-element gap");
    f.radio.setCwPaddleHeld(false);
    tx.startTune();
    check(f.commands == QStringList({"tune:on"}) && tx.isTuning(),
          "releasing the paddle re-admits TUNE");
    tx.stopTune();
}

void delayedReleaseAndReplacement()
{
    Fixture f;
    TransmitModel& tx = f.radio.transmitModel();
    TransmitModel::PttRelease release;
    int cancellations = 0;
    const QMetaObject::Connection cancelled = QObject::connect(&tx, &TransmitModel::pttReleaseCancelled,
        &f.radio, [&] { ++cancellations; });
    tx.setPttOffHook([&](TransmitModel::PttRelease captured) { release = captured; });
    tx.requestPttOn(TransmitModel::PttSource::Mox);
    tx.requestPttOff(TransmitModel::PttSource::Mox);
    check(release.current() && f.commands == QStringList({"mox:on"}), "normal release waits for its tail");
    const TransmitModel::PttRelease old = release;
    tx.requestPttOn(TransmitModel::PttSource::Mox);
    const qsizetype before = f.commands.size();
    old.release();
    check(!old.current() && f.commands.size() == before && tx.isTransmitting(),
          "old RADE-style completion cannot unkey a re-engaged transmission");
    tx.requestPttOff(TransmitModel::PttSource::Mox);
    release.release();
    check(!tx.isTransmitting() && f.commands.back() == "mox:off", "current normal tail releases once");

    tx.requestPttOn(TransmitModel::PttSource::Mox);
    tx.requestPttOff(TransmitModel::PttSource::Mox);
    const TxCoordinator::Operation operation = f.radio.transmitOperation();
    auto replacement = std::make_unique<RecordingBackend>(f.commands);
    f.radio.setBackendForTest(std::move(replacement), QStringLiteral("replacement"));
    f.commands.clear();
    release.release();
    check(!operation.permitsCleanup() && !release.current() && f.commands.isEmpty(),
          "backend replacement fences old operation and delayed release before reuse");
    check(cancellations > 0, "replacement notifies immediate audio/tail cancellation independently of state edges");
    tx.clearPttOffHook();
    QObject::disconnect(cancelled);
}

void flexEncoding()
{
    QStringList commands;
    FlexBackend backend;
    backend.setCommandSink([&](const QString& command) { commands << command; });
    backend.setKeying(true);
    backend.setKeying(false);
    backend.setTune(true, 10);
    backend.setTune(false, 10);
    backend.setAtu(true);
    backend.setAtu(false);
    check(commands == QStringList({"xmit 1", "xmit 0", "transmit tune 1", "transmit tune 0", "atu start", "atu bypass"}),
          "Flex seam preserves exact FlexLib 4.2.18 keying command forms");
}

void teardownAdmission()
{
    for (bool initiallyActive : {false, true}) {
        Fixture f;
        TransmitModel& tx = f.radio.transmitModel();
        if (initiallyActive) {
            tx.setMox(true);
        }
        bool replied = false;
        TxCoordinator::Operation callbackOperation;
        TxOperationIntegrationTestAccess::teardownWithPendingReply(f.radio,
            [&](int code, const QString&) {
                replied = code != 0;
                f.commands.clear();
                tx.setMox(true);
                tx.startTune();
                tx.atuStart();
                f.radio.sendCwKey(true);
                f.radio.sendCwPtt(true);
                f.radio.cwxModel().send("CQ");
                callbackOperation = f.radio.transmitOperation();
                check(f.commands.isEmpty(),
                      "backend teardown reply cannot re-admit any primary key-on intent");
            });
        check(replied, "teardown still answers the pending command with failure");
        check(!callbackOperation.permitsDispatch(std::numeric_limits<qint64>::max()),
              "teardown cannot leave a live operation after its backend dies");
    }
}

void pendingCallbackDisconnectExpiry()
{
    RadioModel radio;
    RadioConnection* const connection = TxOperationIntegrationTestAccess::connection(radio);
    check(connection != nullptr, "default Flex backend exposes its production response connection");
    if (!connection) {
        return;
    }

    const auto deliverResponse = [&](quint32 sequence, int code, const QString& body) {
        const bool invoked = QMetaObject::invokeMethod(
            connection, "commandResponse", Qt::BlockingQueuedConnection,
            Q_ARG(quint32, sequence), Q_ARG(int, code), Q_ARG(QString, body));
        QCoreApplication::sendPostedEvents(&radio, QEvent::MetaCall);
        return invoked;
    };

    int expired = 0;
    int staleCompletions = 0;
    TxOperationIntegrationTestAccess::insertPendingReply(radio, 1234,
        [&](int code, const QString&) {
            if (code != 0) {
                ++expired;
            } else {
                ++staleCompletions;
            }
        });
    TxOperationIntegrationTestAccess::disconnect(radio);
    check(expired == 1 && TxOperationIntegrationTestAccess::pendingReplyCount(radio) == 0,
          "disconnect expires every pending callback before reconnect");

    const bool staleResponseDelivered = deliverResponse(
        1234, 0, QStringLiteral("late reply"));
    check(staleResponseDelivered && expired == 1 && staleCompletions == 0,
          "a late same-sequence response cannot complete a disconnected session callback");

    TxOperationIntegrationTestAccess::disconnect(radio);
    check(expired == 1,
          "a repeated disconnect cannot expire the same callback twice");

    int responseCompletions = 0;
    TxOperationIntegrationTestAccess::insertPendingReply(radio, 1235,
        [&](int, const QString&) {
            ++responseCompletions;
            TxOperationIntegrationTestAccess::disconnect(radio);
        });
    const bool responseDelivered = deliverResponse(
        1235, 0, QStringLiteral("response triggers disconnect"));
    check(responseDelivered && responseCompletions == 1
              && TxOperationIntegrationTestAccess::pendingReplyCount(radio) == 0,
          "response callback can disconnect without re-expiring itself");

    TxOperationIntegrationTestAccess::beginMultiFlexProbe(radio);
    const quint32 probeSequence = TxOperationIntegrationTestAccess::firstPendingReplySequence(radio);
    check(probeSequence != 0, "MultiFlex probe registers its first subscription callback");
    if (probeSequence != 0) {
        TxOperationIntegrationTestAccess::disconnect(radio);
        check(TxOperationIntegrationTestAccess::pendingReplyCount(radio) == 0,
              "an expired MultiFlex subscription cannot enqueue its chained callback");
    }

    radio.setMultiFlexEnabled(true);
    TxOperationIntegrationTestAccess::beginMultiFlexProbe(radio);
    const quint32 radioSubscriptionSequence =
        TxOperationIntegrationTestAccess::firstPendingReplySequence(radio);
    check(radioSubscriptionSequence != 0,
          "MultiFlex probe registers its radio subscription before the client subscription");
    if (radioSubscriptionSequence != 0) {
        const bool radioSubscriptionDelivered = deliverResponse(
            radioSubscriptionSequence, 0, QStringLiteral("radio subscription accepted"));
        const quint32 clientSubscriptionSequence =
            TxOperationIntegrationTestAccess::firstPendingReplySequence(radio);
        check(radioSubscriptionDelivered && clientSubscriptionSequence != 0,
              "accepted radio subscription advances to the client subscription callback");
        if (clientSubscriptionSequence != 0) {
            TxOperationIntegrationTestAccess::disconnect(radio);
            check(TxOperationIntegrationTestAccess::pendingReplyCount(radio) == 0
                      && TxOperationIntegrationTestAccess::hasMultiFlexContinuation(radio),
                  "an expired client subscription cannot continue the MultiFlex handshake");
        }
    }

    bool clientDisconnectContinuationRan = false;
    TxOperationIntegrationTestAccess::disconnectClientsThen(
        radio, {0x10, 0x11}, [&] { clientDisconnectContinuationRan = true; });
    check(TxOperationIntegrationTestAccess::pendingReplyCount(radio) == 1,
          "client-disconnect sequence has one outstanding callback at a time");
    TxOperationIntegrationTestAccess::disconnect(radio);
    check(TxOperationIntegrationTestAccess::pendingReplyCount(radio) == 0
              && !clientDisconnectContinuationRan,
          "disconnect expiration cannot advance the client-disconnect callback chain");
}

void disconnectAdmission()
{
    for (bool force : {false, true}) {
        Fixture f;
        TransmitModel& tx = f.radio.transmitModel();
        tx.setPttOffHook([](TransmitModel::PttRelease) {});
        tx.requestPttOn(TransmitModel::PttSource::Mox);
        tx.requestPttOff(TransmitModel::PttSource::Mox);
        bool cancelled = false;
        const QMetaObject::Connection cancellation = QObject::connect(
            &tx, &TransmitModel::pttReleaseCancelled, &f.radio, [&] {
                cancelled = true;
                tx.atuStart();
            });
        f.commands.clear();
        if (force) {
            f.radio.forceDisconnect();
        } else {
            f.radio.disconnectFromRadio();
        }
        check(cancelled && !f.commands.contains("atu:on"),
              "disconnect closes admission before deferred-release cancellation observers run");
        QObject::disconnect(cancellation);
        tx.clearPttOffHook();
        f.commands.clear();
        // The recorder deliberately still reports connected: real transports
        // can take an event-loop turn or more to deliver their disconnect edge.
        tx.atuStart();
        f.radio.sendCwPtt(true);
        check(f.commands.isEmpty(), "no new TX intent is admitted during the disconnect gap");

        auto replacement = std::make_unique<RecordingBackend>(f.commands);
        replacement->connected = true;
        f.radio.setBackendForTest(std::move(replacement), QStringLiteral("replacement"));
        f.commands.clear();
        tx.atuStart();
        check(f.commands == QStringList({"atu:on"}),
              "a fully installed replacement can admit a fresh operation");
        tx.atuBypass();
    }
}

void reentrantIntents()
{
    Fixture f;
    TransmitModel& tx = f.radio.transmitModel();
    int engaged = 0;
    QObject::connect(&f.radio, &RadioModel::localTransmitEngaged, &f.radio, [&] { ++engaged; });
    tx.setMox(true);
    bool restart = true;
    const QMetaObject::Connection connection = QObject::connect(&tx, &TransmitModel::transmittingChanged,
        &f.radio, [&](bool on) {
            if (!on && restart) {
                restart = false;
                tx.setMox(true);
            }
        });
    f.commands.clear();
    tx.setMox(false);
    check(f.commands == QStringList({"mox:on"}) && tx.isTransmitting() && engaged == 2,
          "reentrant key-on supersedes the old key-up without a stale off command");
    QObject::disconnect(connection);
    tx.setMox(false);

    tx.startTune();
    const QMetaObject::Connection tune = QObject::connect(&tx, &TransmitModel::tuneChanged,
        &f.radio, [&](bool on) { if (!on) { tx.startTune(); } });
    f.commands.clear();
    tx.stopTune();
    check(f.commands == QStringList({"tune:on"}) && tx.isTuning(),
          "a stale TUNE-off cannot stop a reentrant new TUNE intent");
    QObject::disconnect(tune);
    tx.stopTune();

    tx.setMox(true);
    const QMetaObject::Connection observedMox = QObject::connect(&f.radio, &RadioModel::radioTransmittingChanged,
        &f.radio, [&](bool on) { if (!on) { tx.setMox(true); } });
    tx.setMox(false);
    check(tx.isTransmitting() && f.radio.transmitOperation().permitsDispatch(std::numeric_limits<qint64>::max()),
          "command-edge observer re-engage retains the new MOX operation after old cleanup returns");
    QObject::disconnect(observedMox);
    tx.setMox(false);

    tx.startTune();
    const QMetaObject::Connection observedTune = QObject::connect(&f.radio, &RadioModel::radioTransmittingChanged,
        &f.radio, [&](bool on) { if (!on) { tx.startTune(); } });
    tx.stopTune();
    check(tx.isTuning() && f.radio.transmitOperation().permitsDispatch(std::numeric_limits<qint64>::max()),
          "command-edge observer re-engage retains the new TUNE operation after old cleanup returns");
    QObject::disconnect(observedTune);
    tx.stopTune();
}

void quindarNormalRelease()
{
    ClientQuindarTone tone;
    tone.prepare(24000.0);
    tone.setEnabled(true);
    tone.setDurationMs(100);
    Fixture f;
    TransmitModel& tx = f.radio.transmitModel();
    tx.setQuindarTone(&tone);
    tx.requestPttOn(TransmitModel::PttSource::Mox);
    tx.requestPttOff(TransmitModel::PttSource::Mox);
    tx.requestPttOff(TransmitModel::PttSource::Mox);
    check(f.commands == QStringList({"mox:on"}), "duplicate PTT-off does not truncate Quindar outro");
    QEventLoop loop;
    QTimer::singleShot(180, &loop, &QEventLoop::quit);
    loop.exec();
    check(f.commands == QStringList({"mox:on", "mox:off"}), "Quindar normal tail releases exactly once");

    tx.requestPttOn(TransmitModel::PttSource::Mox);
    tx.requestPttOff(TransmitModel::PttSource::Mox);
    tx.requestPttOn(TransmitModel::PttSource::Mox);
    const qsizetype before = f.commands.size();
    QTimer::singleShot(180, &loop, &QEventLoop::quit);
    loop.exec();
    check(f.commands.size() == before && tx.isTransmitting(), "Quindar re-engage cancels old deferred unkey");
    tx.setMox(false);
}

void cwxCancellationFence()
{
    CwxModel cwx;
    int sends = 0;
    bool cleared = false;
    QObject::connect(&cwx, &CwxModel::commandReady, &cwx, [&](const QString& command) {
        if (!cleared && command.startsWith("cwx send")) {
            cleared = true;
            cwx.clearBuffer();
        }
    });
    QObject::connect(&cwx, &CwxModel::transmissionRequested, &cwx,
                     [&](const QString&, int) { ++sends; });
    cwx.send("CQ +TEST DE CALL");
    check(cleared && sends == 0, "CWX cancellation fences remaining segments and local keyer delivery");
}

void cwxCompletionAndRefusal()
{
    {
        CwxModel cwx;
        int operationAdmissions = 0;
        cwx.setSendAvailability([] { return false; });
        cwx.setTransmissionAdmission([&] {
            ++operationAdmissions;
            return CwxModel::TransmissionPermit{[] { return true; }};
        });
        cwx.send(QStringLiteral("CQ"));
        cwx.sendChar(QStringLiteral("E"));
        cwx.sendMacro(1);
        check(operationAdmissions == 0,
              "CWX TUNE refusal happens before acquiring a coordinator operation");
    }
    {
        Fixture f;
        f.backend->caps.hasRadioSideCwKeyer = false;
        f.radio.cwxModel().send("CQ");
        check(f.commands.isEmpty() && !f.radio.transmitOperation().permitsCleanup(),
              "unsupported CWX refuses before acquiring an operation");
    }
    for (const bool reject : {false, true}) {
        Fixture f;
        if (reject) {
            f.backend->cwRejection = QStringLiteral("test rejection");
        }
        int notifications = 0;
        QObject::connect(&f.radio.cwxModel(), &CwxModel::transmissionRequested, &f.radio,
                         [&](const QString&, int) { ++notifications; });
        f.radio.cwxModel().send("CQ +TEST DE CALL");
        check(!f.radio.transmitOperation().permitsDispatch(std::numeric_limits<qint64>::max()),
              "radio-side CWX closes local dispatch after acceptance or rejection");
        check(reject ? notifications == 0 : notifications > 1,
              "rejected CWX neither announces sidetone nor sends later segments");
        if (!reject) {
            check(f.commands.filter("cwx:").size() == notifications,
                  "accepted CWX retains admission through every segment");
        }
    }
    {
        Fixture f;
        f.backend->caps.hasTuner = false;
        f.radio.transmitModel().atuStart();
        check(!f.commands.contains("atu:on")
                  && !f.radio.transmitOperation().permitsDispatch(std::numeric_limits<qint64>::max()),
              "tunerless ATU refuses before acquiring an operation");
    }
    for (const bool observedProgress : {false, true}) {
        Fixture f;
        f.radio.transmitModel().atuStart();
        const TxCoordinator::Operation operation = f.radio.transmitOperation();
        TransmitDelta delta;
        if (observedProgress) {
            delta.atuStatusRaw = QStringLiteral("TUNE_IN_PROGRESS");
            TxOperationIntegrationTestAccess::transmitDelta(f.radio, delta);
        }
        delta.atuStatusRaw = QStringLiteral("TUNE_SUCCESSFUL");
        TxOperationIntegrationTestAccess::transmitDelta(f.radio, delta);
        check(!operation.permitsDispatch(std::numeric_limits<qint64>::max()),
              "terminal ATU status completes local intent even if in-progress was missed");
    }
}

void cwxFailureAndSpeedRestore()
{
    for (const int failure : {0, 1, 2, 3}) {
        CwxModel cwx;
        int cancelled = 0;
        QObject::connect(&cwx, &CwxModel::transmissionCancelled, &cwx, [&] { ++cancelled; });
        const int epoch = cwx.drainEpoch();
        cwx.handleSendReply(0, "10,1", epoch, 3);
        cwx.handleSendReply(failure == 0 ? 1 : 0,
                           failure == 1 ? "bad" : failure == 3 ? "2147483647,1" : "10,1",
                           epoch, failure == 2 ? 0 : 3);
        check(cancelled == 1 && cwx.cwxEndIndex() == -1 && cwx.drainEpoch() != epoch,
              "rejected, malformed, empty or overflowing reply cancels the current CWX batch");
        cwx.handleSendReply(1, {}, epoch, 1);
        check(cancelled == 1, "stale rejected reply cannot cancel a replacement CWX batch");
    }
    CwxModel cwx;
    bool allowed = true;
    QStringList commands;
    cwx.setTransmissionAdmission([&] { return [&] { return allowed; }; });
    QObject::connect(&cwx, &CwxModel::commandReady, &cwx, [&](const QString& command) {
        commands << command;
        if (command == "cwx wpm 23") {
            allowed = false;
        }
    });
    cwx.send("+CQ");
    check(commands == QStringList({"cwx wpm 23", "cwx wpm 20"}),
          "cancelled expansion restores base WPM without sending more text");
}

void flexCwxLifecycle()
{
    QStringList tcp;
    RadioConnection connection;
    Fixture f;
    TxOperationIntegrationTestAccess::injectTcp(f.radio, connection, tcp);
    f.radio.cwxModel().sendMacro(1);
    check(!f.radio.transmitOperation().permitsDispatch(std::numeric_limits<qint64>::max()),
          "unsynced Flex macro closes local handoff without claiming a drain observation");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(tcp.contains("cwx macro send 1"), "unsynced macro preserves radio-side expansion");

    f.radio.cwxModel().send("CQ");
    const TxCoordinator::Operation operation = f.radio.transmitOperation();
    const int epoch = f.radio.cwxModel().drainEpoch();
    check(TxOperationIntegrationTestAccess::cwxDrainArmed(f.radio)
              && operation.permitsDispatch(std::numeric_limits<qint64>::max()),
          "known Flex text keeps its operation until drain or failure");
    f.commands.clear();
    f.radio.transmitModel().startTune();
    check(f.commands.isEmpty() && !f.radio.transmitModel().isTuning(),
          "an in-flight Flex CWX batch refuses TUNE before coordinator re-entry");
    f.radio.cwxModel().handleSendReply(1, {}, epoch, 2);
    check(!TxOperationIntegrationTestAccess::cwxDrainArmed(f.radio)
              && !operation.permitsDispatch(std::numeric_limits<qint64>::max()),
          "Flex reply failure disarms drain and releases the exact local activity");
    f.radio.cwxModel().send("NEW");
    const TxCoordinator::Operation replacement = f.radio.transmitOperation();
    f.radio.cwxModel().handleSendReply(1, {}, epoch, 2);
    check(replacement.permitsDispatch(std::numeric_limits<qint64>::max())
              && TxOperationIntegrationTestAccess::cwxDrainArmed(f.radio),
          "old failed reply cannot release a replacement Flex CWX operation");
    f.radio.cwxModel().handleSendReply(0, "10,1", f.radio.cwxModel().drainEpoch(), 3);
    f.radio.cwxModel().sendMacro(2);
    check(!TxOperationIntegrationTestAccess::cwxDrainArmed(f.radio)
              && f.radio.cwxModel().cwxEndIndex() == -1,
          "unknown-length macro tail cannot be truncated by an earlier batch's drain index");
}

void queuedNetCwEdges()
{
    QList<QByteArray> packets;
    PanadapterStream stream;
    Fixture f;
    TxOperationIntegrationTestAccess::injectNetCwTransport(f.radio, stream,
        [&](const QByteArray& packet) { packets << packet; });
    f.radio.sendCwKeyEdge(true);
    const TxCoordinator::Operation operation = f.radio.transmitOperation();
    f.radio.sendCwKeyEdge(false);
    check(packets.isEmpty() && operation.permitsDispatch(std::numeric_limits<qint64>::max()),
          "normal CW key-up retains authority until already-queued edges have drained");
    QEventLoop loop;
    QTimer::singleShot(60, &loop, &QEventLoop::quit);
    loop.exec();
    int downs = 0;
    int ups = 0;
    for (const QByteArray& packet : packets) {
        downs += packet.mid(28).startsWith("cw key 1 ");
        ups += packet.mid(28).startsWith("cw key 0 ");
    }
    check(downs == 4 && ups == 4, "short CW element retains all four down/up copies through queued delivery");
    check(!operation.permitsDispatch(std::numeric_limits<qint64>::max()),
          "CW operation completes after the final queued key-up reaches the transport");

    packets.clear();
    f.radio.sendCwKeyEdge(true);
    f.radio.forceDisconnect();
    QTimer::singleShot(60, &loop, &QEventLoop::quit);
    loop.exec();
    check(packets.isEmpty(), "disconnect cancels even the first queued NetCW copy before transport dispatch");
}

void queuedCwSessionAndTcpFences()
{
    {
        Fixture f;
        const auto when = std::chrono::steady_clock::now();
        f.radio.queueCwKeyEdge(true, "test", 0, 0, when);
        f.radio.queueCwKeyEdge(false, "test", 0, 0, when);
        f.radio.forceDisconnect();
        auto replacement = std::make_unique<RecordingBackend>(f.commands);
        f.radio.setBackendForTest(std::move(replacement), QStringLiteral("test"));
        f.radio.sendCwKeyEdge(true);
        const TxCoordinator::Operation fresh = f.radio.transmitOperation();
        const qsizetype before = f.commands.size();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        check(f.commands.size() == before && fresh.permitsDispatch(std::numeric_limits<qint64>::max()),
              "queued old-session iambic down/up cannot key or unkey a replacement operation");
        f.radio.queueCwKeyEdge(false, "test", 0, 0, std::chrono::steady_clock::now());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        check(f.commands.last() == "cw:off" && !fresh.permitsDispatch(std::numeric_limits<qint64>::max()),
              "fresh-session queued iambic release still reaches the backend");
    }
    for (const bool withUdp : {false, true}) {
        QStringList tcp;
        QList<QByteArray> udp;
        RadioConnection connection;
        PanadapterStream stream;
        Fixture f;
        TxOperationIntegrationTestAccess::injectTcp(f.radio, connection, tcp);
        if (withUdp) {
            TxOperationIntegrationTestAccess::injectNetCwTransport(f.radio, stream,
                [&](const QByteArray& packet) { udp << packet; });
        }
        f.radio.sendCwKeyEdge(true);
        const TxCoordinator::Operation operation = f.radio.transmitOperation();
        f.radio.sendCwKeyEdge(false);
        check(tcp.isEmpty() && operation.permitsDispatch(std::numeric_limits<qint64>::max()),
              "short NetCW element retains authority until queued TCP delivery");
        QEventLoop loop;
        QTimer::singleShot(60, &loop, &QEventLoop::quit);
        loop.exec();
        check(tcp.size() == 2 && tcp.first().contains(withUdp ? "cw key 1 " : "cw key immediate 1")
                  && tcp.last().contains(withUdp ? "cw key 0 " : "cw key immediate 0")
                  && !operation.permitsDispatch(std::numeric_limits<qint64>::max()),
              "TCP fallback/backstop delivers normal down/up and completes the matching operation");
        tcp.clear();
        udp.clear();
        f.radio.sendCwKeyEdge(true);
        f.radio.forceDisconnect();
        QTimer::singleShot(60, &loop, &QEventLoop::quit);
        loop.exec();
        check(tcp.filter("cw key").isEmpty() && udp.isEmpty(),
              "reset fences queued TCP fallback/backstop and UDP before their final writers");
    }
}
// Both test-injection entry points tear the old backend down, which closes
// admission for the dying session. Neither is followed by an onConnected()
// edge, so each has to drain the latch itself or every later TX intent in that
// test is silently refused and reads as a product bug.
void testInjectionReopensAdmission()
{
    {
        Fixture f;
        check(!TxOperationIntegrationTestAccess::txSessionClosing(f.radio),
              "setBackendForTest reopens admission after tearing the old backend down");
        f.radio.disconnectFromRadio();
        check(TxOperationIntegrationTestAccess::txSessionClosing(f.radio),
              "disconnect closes admission for the dying session");
        auto replacement = std::make_unique<RecordingBackend>(f.commands);
        replacement->connected = true;
        f.radio.setBackendForTest(std::move(replacement), QStringLiteral("replacement"));
        check(!TxOperationIntegrationTestAccess::txSessionClosing(f.radio),
              "a replacement injected after disconnect reopens admission");
    }
    {
        Fixture f;
        f.radio.disconnectFromRadio();
        check(f.radio.rebuildBackendForTest(QStringLiteral("flex"))
                  && !TxOperationIntegrationTestAccess::txSessionClosing(f.radio),
              "rebuildBackendForTest reopens admission like setBackendForTest");
    }
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("tx-operation-integration"));
    if (!settings.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    primaryRoutes();
    refusedStartsAndUnconditionalStops();
    cwTuneMutualExclusion();
    delayedReleaseAndReplacement();
    flexEncoding();
    teardownAdmission();
    pendingCallbackDisconnectExpiry();
    disconnectAdmission();
    reentrantIntents();
    quindarNormalRelease();
    cwxCancellationFence();
    cwxCompletionAndRefusal();
    cwxFailureAndSpeedRestore();
    flexCwxLifecycle();
    queuedNetCwEdges();
    queuedCwSessionAndTcpFences();
    testInjectionReopensAdmission();
    return failures ? 1 : 0;
}
