// Socket-free production-path tests. Only an injected transport recorder is
// used; no radio, peer, listener, discovery or transmitter is opened.
#include "TestSettingsProfile.h"
#include "TxTestAuthority.h"
#include "models/RadioModel.h"
#include "models/TxController.h"
#include "core/SerialPortController.h"
#include "models/SliceModel.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/ClientQuindarTone.h"
#include "core/PanadapterStream.h"
#include "core/RigctlProtocol.h"
#include "core/SmartCatProtocol.h"
#include "core/TciServer.h"
#ifdef HAVE_WEBSOCKETS
#include <QWebSocket>
#endif

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <memory>
#include <limits>
#include <vector>

using namespace AetherSDR;

namespace AetherSDR {
class TxOperationIntegrationTestAccess {
public:
    static void serialPtt(SerialPortController& source, bool down, const TxCoordinator::Request& input = {})
    {
        source.publishPttInput(down, input);
    }
    static void serialClose(SerialPortController& source) { source.retireTxInputs(); }
#ifdef HAVE_WEBSOCKETS
    static void addTciClient(TciServer& server, QWebSocket& socket, RadioModel& radio)
    {
        TciServer::ClientState client;
        client.socket = &socket;
        client.txProducer = radio.registerTxProducer(&socket);
        server.m_clients.append(std::move(client));
    }
    static void tciRequest(TciServer& server, QWebSocket& socket, bool on)
    {
        server.handleTrxRequest(&socket, {0, on, QStringLiteral("tci")});
    }
    static void deferTciRoute(TciServer& server) { server.m_routeTransitionInFlight = true; }
    static void drainTciRoute(TciServer& server)
    {
        server.m_routeTransitionInFlight = false;
        server.drainDeferredRoutingAndPtt();
    }
    static void disconnectTciClient(TciServer& server, QWebSocket& socket)
    {
        server.clientStateFor(&socket)->txProducer.invalidate();
        server.abortTciPtt();
    }
#endif
    static void transmitDelta(RadioModel& radio, const TransmitDelta& delta)
    {
        radio.applyBackendTransmitDelta(delta);
    }
    static bool cwxDrainArmed(const RadioModel& radio) { return radio.m_cwxDrainArmed; }
    static bool txSessionClosing(const RadioModel& radio) { return radio.m_txSessionClosing; }
    static qsizetype pendingReplies(const RadioModel& radio) { return radio.m_pendingCallbacks.size(); }
    static TxCoordinator& coordinator(RadioModel& radio) { return radio.m_txCoordinator; }
    static TxCoordinator::Intent moxIntent(const RadioModel& radio)
    {
        return radio.m_localTxIntents.value(RadioModel::TxActivity::Mox);
    }
    static TxCoordinator::Intent cwIntent(const RadioModel& radio, bool ptt)
    {
        return radio.m_localTxIntents.value(
            ptt ? RadioModel::TxActivity::CwPtt : RadioModel::TxActivity::CwKey);
    }
    static void finishIntent(RadioModel& radio, const TxCoordinator::Intent& intent)
    {
        radio.endLocalTxActivity(intent);
    }
    static void bindTxEncoder(RadioModel& radio, FlexBackend& encoder)
    {
        encoder.setTxCommandSink([&radio](const QString& command, const TxCoordinator::Command& fence) {
            radio.sendTxKeyingCommand(command, fence);
        });
    }
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
    using Writer = std::function<void(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&)>;
    Writer keyingWriter;
    Writer tuneWriter;
    Writer atuWriter;
    std::function<void(const TxCoordinator::Operation&)> cwTextWriter;
    Writer cwTextQueueWriter;
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
    void setKeying(bool on, const TxCoordinator::Operation& operation, const TxCoordinator::Completion& completion) override
    {
        *commands << (on ? "mox:on" : "mox:off");
        if (keyingWriter) {
            keyingWriter(on, operation, completion);
        }
    }
    void setTune(bool on, int, const TxCoordinator::Operation& operation, const TxCoordinator::Completion& completion) override
    {
        *commands << (on ? "tune:on" : "tune:off");
        if (tuneWriter) {
            tuneWriter(on, operation, completion);
        }
    }
    void setAtu(bool on, const TxCoordinator::Operation& operation, const TxCoordinator::Completion& completion) override
    {
        *commands << (on ? "atu:on" : "atu:off");
        if (atuWriter) {
            atuWriter(on, operation, completion);
        }
    }
    void setCwKeying(bool on, bool, int, const AetherSDR::TxCoordinator::Operation&, const AetherSDR::TxCoordinator::Completion&) override { *commands << (on ? "cw:on" : "cw:off"); }
    QString sendCwText(const QString& text, const TxCoordinator::Operation& operation,
                       const TxCoordinator::Completion& completion) override
    {
        *commands << "cwx:" + text;
        if (cwTextWriter) {
            cwTextWriter(operation);
        }
        if (cwTextQueueWriter) {
            cwTextQueueWriter(true, operation, completion);
        }
        return cwRejection;
    }
    void abortCwText(const TxCoordinator::Operation&, const TxCoordinator::Completion&) override { *commands << "cwx:abort"; }
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

void scopedWsprRoutes()
{
    Fixture f;
    f.backend->caps.takesTxAudioOverSeam = true;
    const auto producer = f.radio.registerTxProducer();
    const auto first = producer.request();
    const auto replacement = producer.request();
    TxCoordinator foreign({});
    const auto foreignProducer = foreign.registerProducer();
    check(!f.radio.prepareWsprTransmit(foreignProducer.request()),
          "another engine's input cannot borrow this radio's WSPR route");
    check(f.radio.prepareWsprTransmit(first) && f.radio.hasWsprTxStream(),
          "original WSPR program can prepare its seam audio route");
    check(!f.radio.prepareWsprTransmit(replacement), "another WSPR program cannot overwrite a borrowed route");
    f.radio.releaseWsprTransmit(replacement);
    check(f.radio.hasWsprTxStream(), "foreign WSPR cleanup leaves the original route armed");
    f.radio.releaseWsprTransmit(first);
    check(f.radio.prepareWsprTransmit(replacement), "replacement WSPR program can arm after original release");
    f.radio.releaseWsprTransmit(first);
    check(f.radio.hasWsprTxStream(), "late original WSPR release cannot unarm replacement");
    producer.discardInputs();
    f.radio.releaseWsprTransmit(replacement);
    check(!f.radio.hasWsprTxStream(), "cancelled WSPR input retains only its own route cleanup");
    check(!f.radio.prepareWsprTransmit(replacement), "cancelled WSPR input cannot re-arm the route");
}

void cwxCallbackLifetimes()
{
    for (int stage = 0; stage < 4; ++stage) {
        auto model = std::make_unique<CwxModel>();
        const QPointer<CwxModel> original(model.get());
        if (stage == 0) {
            model->setTransmissionAdmission([&] {
                model.reset();
                return CwxModel::TransmissionPermit([] { return true; });
            });
        } else if (stage == 1 || stage == 3) {
            QObject::connect(model.get(), &CwxModel::commandReady, [&] { model.reset(); });
        } else {
            model->setTextSender([&](const QString&, int) { model.reset(); return true; });
        }
        if (stage == 3) { original->clearBuffer(); }
        else { original->send(QStringLiteral("+CQ TEST")); }
        check(!model, "CW admission, speed dispatch, text delivery and clear tolerate owner destruction");
    }
    auto fixture = std::make_unique<Fixture>();
    fixture->radio.transmitModel().setTxModeGetter([] { return QStringLiteral("CW"); });
    const auto producer = fixture->radio.registerTxProducer();
    const auto input = producer.request();
    fixture->radio.requestProducerCwx(input, QStringLiteral("CQ"), [&] { fixture.reset(); });
    check(!fixture && !input.valid(), "CW admitted callback may tear down the aggregate without later model access");
}

void perIntentCompletion()
{
    Fixture f;
    f.radio.setTransmit(true);
    const auto operation = f.radio.transmitOperation();
    const auto original = TxOperationIntegrationTestAccess::moxIntent(f.radio);
    f.radio.setTransmit(true);
    check(TxOperationIntegrationTestAccess::moxIntent(f.radio).sameIntent(original),
          "repeated production MOX intent does not accumulate hidden holds");
    bool replace = true;
    f.backend->keyingWriter = [&](bool on, const auto&, const auto&) {
        if (!on && replace) {
            replace = false;
            f.radio.setTransmit(true);
        }
    };
    f.radio.setTransmit(false);
    const auto replacement = TxOperationIntegrationTestAccess::moxIntent(f.radio);
    check(replacement.pending() && !replacement.sameIntent(original) && !original.pending(),
          "reentrant MOX reengagement has a new handle while old release retires");
    TxOperationIntegrationTestAccess::finishIntent(f.radio, original);
    check(replacement.pending() && operation.permitsDispatch(std::numeric_limits<qint64>::max()),
          "duplicate old producer release cannot end the reengaged operation");
    f.radio.setTransmit(false);
    check(!replacement.pending() && !operation.permitsDispatch(std::numeric_limits<qint64>::max()),
          "new producer release drains normally with no orphaned prior hold");

    f.radio.setTransmit(true);
    const auto shared = f.radio.transmitOperation();
    auto& coordinator = TxOperationIntegrationTestAccess::coordinator(f.radio);
    const auto additional = coordinator.beginIntent(shared, {}, TxCoordinator::Activity::Mox);
    f.radio.setTransmit(false);
    check(additional.pending() && shared.permitsDispatch(std::numeric_limits<qint64>::max()),
          "production MOX completion does not erase a second producer contribution");
    TxOperationIntegrationTestAccess::finishIntent(f.radio, additional);
    check(!shared.permitsDispatch(std::numeric_limits<qint64>::max()),
          "last captured producer completion ends only local intent");

    f.radio.setTransmit(true);
    const auto cwOperation = f.radio.transmitOperation();
    const auto olderCw = coordinator.beginIntent(cwOperation, {}, TxCoordinator::Activity::CwKey);
    check(coordinator.requestIntentEnd(olderCw), "earlier CW contribution enters local drain");
    const auto newerCw = coordinator.beginIntent(cwOperation, olderCw, TxCoordinator::Activity::CwKey);
    TxOperationIntegrationTestAccess::finishIntent(f.radio, newerCw);
    f.commands.clear();
    f.radio.transmitModel().startTune();
    check(!f.commands.contains(QStringLiteral("tune:on")) && !f.radio.transmitModel().isTuning(),
          "earlier draining CW keeps production TUNE interlock closed after a newer edge ends");
    TxOperationIntegrationTestAccess::finishIntent(f.radio, olderCw);
    f.radio.transmitModel().startTune();
    check(f.commands.contains(QStringLiteral("tune:on")),
          "TUNE becomes available after all local CW contributions drain");
    f.radio.transmitModel().stopTune();
    f.radio.setTransmit(false);
}

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

void localCompletionDoesNotAuthorizeHandoff()
{
    Fixture f;
    TxCoordinator& coordinator = TxOperationIntegrationTestAccess::coordinator(f.radio);
    const TxCoordinator::Actor competitor = coordinator.registerActor({true, 0});
    f.radio.setTransmit(true);
    const TxCoordinator::Operation first = f.radio.transmitOperation();
    f.radio.setTransmit(false);
    check(!first.permitsDispatch(std::numeric_limits<qint64>::max()),
          "production MOX release fences dispatch after local completion");
    check(coordinator.acquire(competitor, std::numeric_limits<qint64>::max()).refusal
              == TxCoordinator::Refusal::Busy,
          "production completion cannot lend the radio to an independent actor");
    f.radio.setTransmit(true);
    const TxCoordinator::Operation second = f.radio.transmitOperation();
    check(!first.sameOperation(second) && second.permitsDispatch(std::numeric_limits<qint64>::max()),
          "normal desktop reengagement remains available without a new duration cap");
    check(!coordinator.acknowledgeStopped(first), "old local completion cannot acknowledge reengaged desktop TX");
    f.radio.setTransmit(false);
    // Uncorrelated radio RX status must not be upgraded into qualified handoff.
    TransmitDelta idle;
    idle.mox = false;
    idle.tune = false;
    TxOperationIntegrationTestAccess::transmitDelta(f.radio, idle);
    emit f.backend->keyingStateConfirmed(false);
    check(coordinator.acquire(competitor, std::numeric_limits<qint64>::max()).refusal
              == TxCoordinator::Refusal::Busy,
          "uncorrelated RX state does not release an unconfirmed owner");
    TxOperationIntegrationTestAccess::teardownWithPendingReply(f.radio, [](quint32, const QString&) {});
    check(!coordinator.recovering() && !second.permitsCleanup(),
          "production backend teardown acknowledges the retained owner and retires its generation");
    const TxCoordinator::Admission afterTeardown = coordinator.acquire(competitor, std::numeric_limits<qint64>::max());
    check(afterTeardown.accepted(), "transport teardown clears the old ownership barrier");
    check(coordinator.finishLocalIntent(afterTeardown.operation)
              && coordinator.acknowledgeStopped(afterTeardown.operation),
          "test-only admission is retired without dispatching keying");
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
    TxTestAuthority authority;
    QStringList commands;
    FlexBackend backend;
    backend.setCommandSink([&](const QString& command) { commands << command; });
    backend.setKeying(true, authority.operation);
    backend.setTune(true, 10, authority.operation);
    backend.setAtu(true, authority.operation);
    check(commands.isEmpty(), "primary Flex keying never falls back to an unfenced generic sink");
    std::vector<bool> keying;
    backend.setTxCommandSink([&](const QString& command, const TxCoordinator::Command& fence) {
        commands << command;
        keying.push_back(fence.keying);
    });
    backend.setKeying(true, authority.operation);
    backend.setKeying(false, authority.operation);
    backend.setTune(true, 10, authority.operation);
    backend.setTune(false, 10, authority.operation);
    backend.setAtu(true, authority.operation);
    backend.setAtu(false, authority.operation);
    backend.abortCwText(authority.operation);
    check(commands == QStringList({"xmit 1", "xmit 0", "transmit tune 1", "transmit tune 0", "atu start", "atu bypass", "cwx clear"}),
          "Flex seam preserves exact FlexLib 4.2.18 keying command forms");
    check(keying == std::vector<bool>({true, false, true, false, true, false, false}),
          "Flex encoder distinguishes keying from cleanup without claiming authority");
}

void queuedPrimaryKeying()
{
    for (int testCase = 0; testCase != 12; ++testCase) {
        const int kind = testCase / 4; // MOX, TUNE, ATU
        const int scenario = testCase % 4;
        // The terminal connection is inert: no init, connect or socket bind.
        // Flex encodes the commands; the existing injected backend supplies
        // the model's admission prerequisites without synthetic firmware.
        QStringList wire;
        RadioConnection connection;
        FlexBackend encoder;
        Fixture f;
        TxOperationIntegrationTestAccess::injectTcp(f.radio, connection, wire);
        TxOperationIntegrationTestAccess::bindTxEncoder(f.radio, encoder);
        f.backend->keyingWriter = [&encoder](bool on, const auto& operation, const auto& completion) {
            encoder.setKeying(on, operation, completion);
        };
        f.backend->tuneWriter = [&encoder](bool on, const auto& operation, const auto& completion) {
            encoder.setTune(on, 10, operation, completion);
        };
        f.backend->atuWriter = [&encoder](bool on, const auto& operation, const auto& completion) {
            encoder.setAtu(on, operation, completion);
        };
        const auto setKeying = [&](bool on) {
            if (kind == 0) {
                f.radio.setTransmit(on);
            } else if (kind == 1) {
                if (on) {
                    f.radio.transmitModel().startTune();
                } else {
                    f.radio.transmitModel().stopTune();
                }
            } else if (on) {
                f.radio.transmitModel().atuStart();
            } else {
                f.radio.transmitModel().atuBypass();
            }
        };
        const QString on = kind == 0 ? "xmit 1" : kind == 1 ? "transmit tune 1" : "atu start";
        const QString off = kind == 0 ? "xmit 0" : kind == 1 ? "transmit tune 0" : "atu bypass";
        if (scenario == 3) {
            setKeying(false); // old idle cleanup must not unkey a new owner
        }
        setKeying(true);
        const TxCoordinator::Operation operation = f.radio.transmitOperation();
        if (scenario == 0 || scenario == 2) {
            setKeying(false);
        }
        if (scenario == 1) {
            f.radio.forceDisconnect();
        } else if (scenario == 2) {
            setKeying(true);
        }
        QEventLoop loop;
        QTimer::singleShot(0, &loop, &QEventLoop::quit);
        loop.exec();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        if (scenario == 0) {
            check(wire == QStringList({on, off}),
                  "short normal primary TX preserves both queued edges");
            check(!operation.permitsDispatch(std::numeric_limits<qint64>::max()),
                  "normal completion follows the terminal cleanup queue barrier");
        } else if (scenario == 1) {
            check(!wire.contains(on), "reset cancels queued primary TX before the terminal writer");
        } else if (scenario == 2) {
            check(wire == QStringList({on, off, on})
                      && f.radio.transmitOperation().permitsDispatch(std::numeric_limits<qint64>::max()),
                  "old cleanup completion cannot finish a reengaged local TX intent");
        } else {
            check(wire == QStringList({on}), "idle cleanup cannot unkey the newly acquired operation");
        }
    }
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
    const auto oldBatch = cwx.queuedTransmissionPermit();
    cwx.resetDrainWatch();
    check(!oldBatch() && cwx.queuedTransmissionPermit()(),
          "CWX reset invalidates worker-safe old batch permits only");
    CwxModel::TransmissionPermit destroyed;
    {
        CwxModel temporary;
        destroyed = temporary.queuedTransmissionPermit();
    }
    check(!destroyed(), "a queued CWX permit cannot outlive its producer");
}

void queuedCwxCancellation()
{
    {
        Fixture f;
        TxCoordinator::Operation queued;
        TxCoordinator::Completion completion;
        f.backend->cwTextQueueWriter = [&](bool, const TxCoordinator::Operation& operation,
                                           const TxCoordinator::Completion& delivered) {
            queued = operation;
            completion = delivered;
        };
        f.radio.setTransmit(true);
        const TxCoordinator::Operation mox = f.radio.transmitOperation();
        f.radio.cwxModel().send("OLD");
        check(queued.permitsDispatch(TxCoordinator::monotonicMs()),
              "typed CW sender retains the original batch during queue handoff");
        f.radio.cwxModel().clearBuffer();
        check(!queued.permitsDispatch(TxCoordinator::monotonicMs())
                  && mox.permitsDispatch(TxCoordinator::monotonicMs()),
              "typed backend CW cancellation is independent of a held MOX operation");
        completion.finish();
    }
    for (int scenario = 0; scenario != 3; ++scenario) {
        QStringList tcp;
        RadioConnection connection;
        Fixture f;
        TxOperationIntegrationTestAccess::injectTcp(f.radio, connection, tcp);
        f.radio.setTransmit(true);
        const TxCoordinator::Operation heldMox = f.radio.transmitOperation();
        f.radio.cwxModel().send("OLD +BATCH");
        if (scenario == 0) {
            f.radio.cwxModel().clearBuffer();
        } else if (scenario == 1) {
            f.radio.cwxModel().clearBuffer();
            f.radio.cwxModel().send("NEW");
        } else {
            f.radio.forceDisconnect();
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        check(tcp.filter("cwx send").size() == (scenario == 1 ? 1 : 0)
                  && (scenario != 1 || tcp.filter("cwx send").first().contains("NEW")),
              "clear/reset cancels queued CWX segments without borrowing a held MOX permit");
        check(TxOperationIntegrationTestAccess::pendingReplies(f.radio) == (scenario == 1 ? 1 : 0),
              "cancelled queued CWX retires its reply callback, leaving only replacement work");
        if (scenario != 2) {
            check(heldMox.sameOperation(f.radio.transmitOperation())
                      && heldMox.permitsDispatch(std::numeric_limits<qint64>::max()),
                  "CWX cancellation does not release the separately held MOX intent");
        }
    }
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
    check(f.radio.transmitOperation().permitsDispatch(std::numeric_limits<qint64>::max()),
          "unsynced Flex macro retains authority until terminal dispatch");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(!f.radio.transmitOperation().permitsDispatch(std::numeric_limits<qint64>::max()),
          "unsynced Flex macro closes local handoff without claiming a drain observation");
    check(tcp.contains("cwx macro send 1"), "unsynced macro preserves radio-side expansion");

    f.radio.cwxModel().send("CQ");
    const TxCoordinator::Operation operation = f.radio.transmitOperation();
    const int epoch = f.radio.cwxModel().drainEpoch();
    const auto queuedBatch = f.radio.cwxModel().queuedTransmissionPermit();
    check(TxOperationIntegrationTestAccess::cwxDrainArmed(f.radio)
              && operation.permitsDispatch(std::numeric_limits<qint64>::max()),
          "known Flex text keeps its operation until drain or failure");
    f.commands.clear();
    f.radio.transmitModel().startTune();
    check(f.commands.isEmpty() && !f.radio.transmitModel().isTuning(),
          "an in-flight Flex CWX batch refuses TUNE before coordinator re-entry");
    f.radio.cwxModel().handleSendReply(1, {}, epoch, 2);
    check(!TxOperationIntegrationTestAccess::cwxDrainArmed(f.radio)
              && !queuedBatch(),
          "Flex reply failure immediately disarms drain and fences the exact queued batch");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(!operation.permitsDispatch(std::numeric_limits<qint64>::max())
              && tcp.contains("cwx clear") && tcp.filter("cwx send").isEmpty(),
          "failed Flex batch completes after queued cleanup without writing cancelled text");
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
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(tcp.filter("cwx send").size() == 1
              && tcp.filter("cwx send").first().contains("NEW")
              && tcp.contains("cwx macro send 2"),
          "abandoning an unknown-length drain watch preserves queued text and macro tail");
}

void overlappingCwContributions()
{
    for (const bool withUdp : {false, true}) {
        for (int route = 0; route != 3; ++route) {
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
            const auto send = [&](bool down) {
                if (route == 0) {
                    f.radio.sendCwKey(down);
                } else if (route == 1) {
                    f.radio.sendCwKeyEdge(down);
                } else {
                    f.radio.sendCwPtt(down);
                }
            };
            send(true);
            const auto operation = f.radio.transmitOperation();
            const auto first = TxOperationIntegrationTestAccess::cwIntent(f.radio, route == 2);
            send(false);
            send(true);
            const auto replacement = TxOperationIntegrationTestAccess::cwIntent(f.radio, route == 2);
            check(first.pending() && replacement.pending() && !first.sameIntent(replacement),
                  "queued CW reengagement retains distinct old and current contributions");
            QEventLoop loop;
            QTimer::singleShot(60, &loop, &QEventLoop::quit);
            loop.exec();
            check(!first.pending() && replacement.pending()
                      && operation.permitsDispatch(std::numeric_limits<qint64>::max()),
                  "old CW queue completion retires only its captured contribution");
            check(tcp.size() == 3 && (!withUdp || udp.size() == 12),
                  "reengagement preserves normal queued down/up/down transport delivery");
            send(false);
            QTimer::singleShot(60, &loop, &QEventLoop::quit);
            loop.exec();
            check(!replacement.pending() && !operation.permitsDispatch(std::numeric_limits<qint64>::max())
                      && tcp.size() == 4 && (!withUdp || udp.size() == 16),
                  "final CW release drains all contributions without an orphaned hold");
        }
    }
}

void disconnectDuringEnteredWrite()
{
    Fixture f;
    f.radio.setTransmit(true);
    const auto operation = f.radio.transmitOperation();
    auto dispatch = operation.beginDispatch(0, false);
    check(bool(dispatch), "disconnect fixture holds an entered terminal write");
    TxOperationIntegrationTestAccess::teardownWithPendingReply(f.radio, {});
    auto& coordinator = TxOperationIntegrationTestAccess::coordinator(f.radio);
    check(coordinator.recovering() && coordinator.hasInFlightDispatches(),
          "transport teardown retains recovery while an entered writer returns");
    dispatch = {};
    QEventLoop loop;
    QTimer::singleShot(30, &loop, &QEventLoop::quit);
    loop.exec();
    check(!coordinator.recovering() && !coordinator.hasInFlightDispatches(),
          "teardown acknowledgment completes after dispatch return without a permanent latch");
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
void scopedCompatibilityStop()
{
    Fixture f;
    f.radio.cwxModel().send("CQ");
    const TxCoordinator::Operation text = f.radio.transmitOperation();
    check(!text.permitsDispatch(std::numeric_limits<qint64>::max()) && text.permitsCleanup(),
          "radio-side text handoff retains only cleanup, not key-on authority");
    f.commands.clear();
    f.radio.requestTransmitStop(text);
    check(f.commands.contains("cwx:abort") && !f.commands.contains("atu:off"),
          "captured stop aborts a handed-off text tail without changing tuner configuration");
    f.radio.setTransmit(true);
    f.commands.clear();
    f.radio.requestTransmitStop(text);
    f.radio.requestTransmitStop({});
    check(f.commands.isEmpty(), "stale and empty stop handles cannot affect replacement TX");
}

void operatorCancelsCapturedInputs()
{
    Fixture f;
    const auto first = std::make_shared<TxController>(&f.radio);
    const auto second = std::make_shared<TxController>(&f.radio);
    const TxController::Input queued = second->capture(TxController::Activity::Tune);
    const TxController::Input mox = first->capture(TxController::Activity::Mox);
    check(mox.start(), "operator-cancel fixture admits scoped MOX");
    const TxController::Input other = second->capture(TxController::Activity::Mox);
    check(other.start(), "operator-cancel fixture admits another compatible producer");
    const TxCoordinator::Operation original = f.radio.transmitOperation();
    bool nestedAccepted = false;
    f.backend->keyingWriter = [&](bool on, const TxCoordinator::Operation&, const TxCoordinator::Completion&) {
        if (!on) {
            nestedAccepted = f.radio.localTxController()->capture(TxController::Activity::Mox).start();
        }
    };
    f.commands.clear();
    f.radio.cancelLocalTransmit();
    f.backend->keyingWriter = {};
    check(!nestedAccepted && f.commands.contains("mox:off") && !f.commands.contains("mox:on"),
          "operator cancel unkeys all scoped contributors before reentrant admission");
    check(!mox.valid() && !other.valid() && !queued.start()
              && !original.permitsDispatch(TxCoordinator::monotonicMs()),
          "operator cancel fences both admitted contributions and unadmitted queued inputs");
    const TxController::Input fresh = f.radio.localTxController()->capture(TxController::Activity::Mox);
    check(fresh.start(), "fresh operator intent may reengage after the local stop returns");
    f.commands.clear();
    mox.stop();
    other.stop();
    f.radio.requestTransmitStop(original);
    check(f.commands.isEmpty() && fresh.valid(), "old release callbacks cannot unkey fresh operator intent");
    fresh.stop();

    const auto idle = std::make_shared<TxController>(&f.radio);
    const TxController::Input pending = idle->capture(TxController::Activity::Mox);
    f.radio.cancelLocalTransmit();
    check(!pending.start(), "cancel while idle also discards pre-admission captured input");
}

void derivedInputsStayWithTheirProducer()
{
    Fixture f;
    const auto controller = std::make_shared<TxController>(&f.radio);
    const TxCoordinator::Request input = controller->capture(TxController::Activity::CwKey).request();
    const TxCoordinator::Request element = input.derive();
    check(element.valid() && !element.derive().valid(), "sequencer inputs have one bounded derivation level");
    check(f.radio.requestProducerCw(element, true) && controller->hasWork(),
          "derived CW admission remains visible to its original controller");
    const TxCoordinator::Context media = f.radio.captureTxMedia(element);
    f.commands.clear();
    controller->invalidate();
    check(f.commands.contains("cw:off") && !media.permitsDispatch(TxCoordinator::monotonicMs())
              && !input.derive().valid(),
          "producer invalidation unkeys its derived element and fences future elements");
    const auto fresh = std::make_shared<TxController>(&f.radio);
    const TxCoordinator::Request pending = fresh->capture(TxController::Activity::CwKey).request();
    const TxCoordinator::Request queued = pending.derive();
    f.radio.cancelLocalTransmit();
    f.commands.clear();
    check(!f.radio.requestProducerCw(queued, true) && f.commands.isEmpty(),
          "cancelled parent input cannot admit its previously derived queued element");
}

void queuedDerivedCwElements()
{
    QStringList tcp;
    RadioConnection connection;
    Fixture f;
    TxOperationIntegrationTestAccess::injectTcp(f.radio, connection, tcp);
    const TxCoordinator::Request input = f.radio.registerTxProducer().request();
    for (int i = 0; i != 2; ++i) {
        const TxCoordinator::Request element = input.derive();
        check(f.radio.requestProducerCw(element, true), "queued sequencer element admits its original request");
        (void)f.radio.requestProducerCw(element, false);
    }
    QEventLoop loop;
    QTimer::singleShot(60, &loop, &QEventLoop::quit);
    loop.exec();
    check(tcp.filter("cw key immediate 1").size() == 2 && tcp.filter("cw key immediate 0").size() == 2,
          "two normally released derived elements retain both queued down/up pairs");
}

void deviceCloseFencesOriginalInputs()
{
    Fixture f;
    const TxCoordinator::Producer device = f.radio.registerTxProducer();
    const TxCoordinator::Request queued = device.request();
    const TxCoordinator::Request held = device.request();
    check(f.radio.setProducerTransmit(held, true), "device input is admitted before close");
    const TxCoordinator::Context original = f.radio.captureTxMedia(held);
    device.discardInputs();
    check(device.valid() && !held.valid() && !queued.valid()
              && !original.permitsDispatch(TxCoordinator::monotonicMs()),
          "device close fences every captured input and media without destroying the reusable producer");
    f.radio.abortTxProducerInputs(device, TransmitModel::PttSource::Mox);
    check(f.commands.last() == "mox:off", "device close still releases original admitted PTT");
    const TxCoordinator::Request fresh = device.request();
    check(f.radio.setProducerTransmit(fresh, true), "fresh raw device input can start after close cleanup");
    f.commands.clear();
    (void)f.radio.setProducerTransmit(held, false);
    check(f.commands.isEmpty(), "old device key-up cannot stop a replacement input");
    f.radio.setProducerTransmit(fresh, false);
}

void compoundAndDeviceControllerScopes()
{
    Fixture f;
    const auto source = std::make_shared<TxController>(&f.radio);
    const auto held = source->capture(TxController::Activity::Mox);
    check(held.start(), "source hold starts before a compound pointer input");
    const auto abandoned = TxController::captureInputScope(source);
    check(abandoned && abandoned->sameController(source), "compound view retains its source identity");
    abandoned->invalidate();
    check(held.active() && held.valid(), "abandoned compound input cannot cancel an earlier source hold");
    held.stop();

    TxController::Input queued;
    {
        const auto scope = TxController::captureInputScope(source);
        queued = scope->capture(TxController::Activity::Mox);
    }
    check(queued.start(), "normal input-view destruction retains an admitted queued action");
    queued.stop();
    const auto cancelled = TxController::captureInputScope(source);
    f.radio.cancelLocalTransmit();
    check(!cancelled->capture(TxController::Activity::Mox).valid(),
          "second compound activation cannot remint after operator cancellation");

    QObject device;
    const auto native = TxController::forNativeDevice(&f.radio, &device);
    check(!native->capture(TxController::Activity::Mox).valid()
              && !TxController::captureInputScope(native),
          "device cannot capture authority at callback time without original raw input");
    const auto raw = native->captureRawInput();
    native->discardDeviceInputs();
    const auto stale = TxController::captureInputScope(native, raw);
    check(stale && !stale->capture(TxController::Activity::Mox).valid(),
          "device close before queued delivery rejects the old raw input");
    const auto fresh = TxController::captureInputScope(native, native->captureRawInput());
    const auto replacement = fresh->capture(TxController::Activity::Mox);
    check(replacement.start(), "new raw device input starts after close");
    stale->current(TxController::Activity::Mox).stop();
    check(replacement.active(), "old device epoch release cannot consume replacement hold");
    const auto foreign = std::make_shared<TxController>(&f.radio);
    check(!TxController::captureInputScope(foreign, native->captureRawInput()),
          "a raw input cannot be relabeled as a different controller");
    native->discardDeviceInputs();
    native->cleanupDeviceInputs();
    check(!replacement.active(), "device close cleans only original producer work");
}

void serialInputQueueLifetimes()
{
    Fixture f;
    SerialPortController source;
    const TxCoordinator::Producer producer = f.radio.registerTxProducer(&source);
    source.setTxProducer(producer);
    QObject::connect(&source, &SerialPortController::externalPttChanged, &f.radio,
        [&](bool down, const TxCoordinator::Request& input) {
            (void)f.radio.setProducerTransmit(input, down, TransmitModel::PttSource::Mox);
        }, Qt::QueuedConnection);
    QObject::connect(&source, &SerialPortController::txInputsCancelled, &f.radio, [&] {
        f.radio.abortTxProducerInputs(producer, TransmitModel::PttSource::Mox);
    }, Qt::QueuedConnection);
    const auto drain = [] { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); };
    TxOperationIntegrationTestAccess::serialPtt(source, true, producer.request());
    TxOperationIntegrationTestAccess::serialClose(source);
    drain();
    check(f.commands.isEmpty(), "serial close before queued delivery cannot admit an old press");
    TxOperationIntegrationTestAccess::serialPtt(source, true, producer.request());
    drain();
    check(f.commands.contains("mox:on"), "fresh serial input after device close captures a new request");
    TxOperationIntegrationTestAccess::serialPtt(source, false);
    drain();
    check(f.commands.last() == "mox:off", "serial key-up releases its original input across the queue");

    TxOperationIntegrationTestAccess::serialPtt(source, true, producer.request());
    f.radio.forceDisconnect();
    auto replacement = std::make_unique<RecordingBackend>(f.commands);
    f.radio.setBackendForTest(std::move(replacement), QStringLiteral("test"));
    f.commands.clear();
    drain();
    check(f.commands.isEmpty(), "serial press captured before reconnect cannot acquire in replacement session");
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
void protocolProducerLifetimes()
{
    const auto drain = [] {
        // All transports are injected on this thread. Drain the real queued
        // hops (input -> engine -> writer -> completion), not a fake peer.
        for (int i = 0; i != 6; ++i) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        }
    };
    QStringList wire;
    RadioConnection connection;
    FlexBackend encoder;
    Fixture f;
    TxOperationIntegrationTestAccess::injectTcp(f.radio, connection, wire);
    TxOperationIntegrationTestAccess::bindTxEncoder(f.radio, encoder);
    f.backend->keyingWriter = [&encoder](bool on, const auto& operation, const auto& completion) {
        encoder.setKeying(on, operation, completion);
    };
    {
        RigctlProtocol abandoned(&f.radio);
        check(abandoned.handleLine("T 1") == "RPRT 0\n", "rigctl accepts a syntactically valid queued PTT request");
    }
    drain();
    check(wire.isEmpty(), "destroying the accepted CAT session fences its not-yet-admitted key-on");
    {
        SmartCatProtocol disconnected(&f.radio);
        (void)disconnected.processCommand("TX");
        disconnected.releasePtt();
        drain();
        check(!wire.contains("xmit 1"), "SmartCAT disconnect fences queued key-on before protocol destruction");
        wire.clear();
    }
    {
        RigctlProtocol first(&f.radio);
        RigctlProtocol second(&f.radio);
        (void)first.handleLine("T 1");
        (void)second.handleLine("T 1");
        drain();
        const auto operation = f.radio.transmitOperation();
        wire.clear();
        (void)first.handleLine("T 0");
        drain();
        check(wire.isEmpty() && operation.permitsDispatch(TxCoordinator::monotonicMs()),
              "one rigctl client's release leaves another client's contribution active");
        (void)first.handleLine("T 0");
        drain();
        check(wire.isEmpty(), "duplicate rigctl release cannot unkey another client");
        (void)second.handleLine("T 0");
        drain();
        check(wire == QStringList{"xmit 0"} && !operation.permitsDispatch(TxCoordinator::monotonicMs()),
              "last rigctl contribution releases once and completes after terminal delivery");
        wire.clear();
        (void)first.handleLine("T 1");
        (void)first.handleLine("T 0");
        drain();
        check(wire == QStringList({"xmit 1", "xmit 0"}),
              "producer-scoped short rigctl on/off retains both normal queued edges");
    }
    drain();
    wire.clear();
    {
        SmartCatProtocol remaining(&f.radio);
        {
            SmartCatProtocol departing(&f.radio);
            (void)departing.processCommand("TX");
            (void)remaining.processCommand("TX");
            drain();
            wire.clear();
        }
        drain();
        check(wire.isEmpty() && f.radio.transmitOperation().permitsDispatch(TxCoordinator::monotonicMs()),
              "SmartCAT session teardown cannot release the remaining client's PTT");
        (void)remaining.processCommand("RX");
        drain();
        check(wire == QStringList{"xmit 0"}, "SmartCAT release uses its own captured request");
    }
    drain();
}

void producerNormalTails()
{
    Fixture f;
    QObject owner;
    const TxCoordinator::Producer producer = f.radio.registerTxProducer(&owner);
    const TxCoordinator::Request first = producer.request();
    TransmitModel::PttRelease tail;
    f.radio.transmitModel().setPttOffHook([&tail](TransmitModel::PttRelease release) {
        tail = std::move(release);
    });
    check(f.radio.requestProducerPttOn(first, TransmitModel::PttSource::TciHardware),
          "scoped hardware PTT uses the normal preflight");
    const TxCoordinator::Context firstMedia = f.radio.captureTxMedia(first);
    f.radio.requestProducerPttOff(first, TransmitModel::PttSource::TciHardware);
    f.radio.requestProducerPttOff(first, TransmitModel::PttSource::TciHardware);
    check(tail.current() && firstMedia.permitsDispatch(TxCoordinator::monotonicMs())
              && f.commands == QStringList{"mox:on"},
          "scoped normal release retains media through one delayed tail");
    const TransmitModel::PttRelease staleTail = tail;
    const TxCoordinator::Request second = producer.request();
    check(f.radio.requestProducerPttOn(second, TransmitModel::PttSource::TciHardware),
          "fresh scoped request supersedes an in-flight normal tail");
    const TxCoordinator::Context secondMedia = f.radio.captureTxMedia(second);
    staleTail.release();
    check(!staleTail.current() && !firstMedia.permitsDispatch(TxCoordinator::monotonicMs())
              && secondMedia.permitsDispatch(TxCoordinator::monotonicMs())
              && f.commands == QStringList({"mox:on", "mox:on"}),
          "superseded tail retires only its original producer intent");
    f.radio.requestProducerPttOff(second, TransmitModel::PttSource::TciHardware);
    tail.release();
    tail.release();
    check(f.commands == QStringList({"mox:on", "mox:on", "mox:off"})
              && !secondMedia.permitsDispatch(TxCoordinator::monotonicMs()),
          "scoped tail completes exactly once and ends its media authority");

    const TxCoordinator::Request third = producer.request();
    const TxCoordinator::Request fourth = producer.request();
    check(f.radio.requestProducerPttOn(third, TransmitModel::PttSource::TciHardware)
              && f.radio.setProducerTransmit(fourth, true), "compatible contributors can overlap");
    f.commands.clear();
    f.radio.abortProducerPtt(third, TransmitModel::PttSource::TciHardware);
    f.radio.abortProducerPtt(third, TransmitModel::PttSource::TciHardware);
    check(f.commands.isEmpty()
              && f.radio.captureTxMedia(fourth).permitsDispatch(TxCoordinator::monotonicMs()),
          "teardown and late-edge retries cannot unkey another contributor");
    f.radio.setProducerTransmit(fourth, false);
}

void producerCwxQueue()
{
    Fixture f;
    const TxCoordinator::Producer producer = f.radio.registerTxProducer();
    const TxCoordinator::Request request = producer.request();
    TxCoordinator::Operation queued;
    TxCoordinator::Completion completion;
    f.backend->cwTextQueueWriter = [&](bool, const auto& operation, const auto& done) {
        queued = operation;
        completion = done;
    };
    check(f.radio.requestProducerCwx(request, QStringLiteral("TEST"))
              && queued.permitsDispatch(TxCoordinator::monotonicMs()),
          "scoped CW text retains its original producer through queued handoff");
    const qsizetype before = f.commands.size();
    const TxCoordinator::Request competitor = f.radio.registerTxProducer().request();
    check(!f.radio.requestProducerCwx(competitor, QStringLiteral("OTHER"))
              && !competitor.valid() && f.commands.size() == before,
          "another CW producer cannot replace or append to a live queue");
    completion.finish();
    check(!queued.permitsDispatch(TxCoordinator::monotonicMs()),
          "scoped CW handoff completes only after its terminal writer returns");
    f.radio.abortProducerCwx(request);
    const qsizetype stopped = f.commands.size();
    f.radio.abortProducerCwx(request);
    check(f.commands.last() == QStringLiteral("cwx:abort") && f.commands.size() == stopped,
          "scoped CW tail abort is available after local handoff and idempotent");
    const TxCoordinator::Request fresh = producer.request();
    check(f.radio.requestProducerCwx(fresh, QStringLiteral("NEXT")), "fresh CW input can start a new batch");
    const qsizetype replacement = f.commands.size();
    f.radio.abortProducerCwx(request);
    check(f.commands.size() == replacement && queued.permitsDispatch(TxCoordinator::monotonicMs()),
          "old CW abort cannot consume a replacement producer request");
    producer.invalidate();
    check(!queued.permitsDispatch(TxCoordinator::monotonicMs()),
          "producer teardown immediately fences queued CW before owner-thread cleanup");
    completion.finish();
}

void nativeCwxUsesOperatorProducer()
{
    Fixture f;
    TxCoordinator::Operation queued;
    TxCoordinator::Completion completion;
    f.backend->cwTextQueueWriter = [&](bool, const auto& operation, const auto& done) {
        queued = operation;
        completion = done;
    };
    const auto controller = f.radio.localTxController();
    f.radio.cwxModel().send(QStringLiteral("CQ"));
    check(controller->hasWork() && queued.permitsDispatch(TxCoordinator::monotonicMs()),
          "native CW text is captured by the shared operator producer before backend queues");
    controller->invalidate();
    check(f.commands.contains("cwx:abort") && !queued.permitsDispatch(TxCoordinator::monotonicMs()),
          "operator producer teardown also fences native CW text and aborts its own queue");
    completion.finish();
}

void cwQueueDoesNotOwnManualPtt()
{
    for (const bool scoped : {false, true}) {
        Fixture f;
        const auto controller = std::make_shared<TxController>(&f.radio);
        const TxController::Input hold = controller->capture(TxController::Activity::Mox);
        if (scoped) {
            check(hold.start(), "scoped MOX is admitted before queued CW");
        } else {
            f.radio.setTransmit(true);
        }
        TxCoordinator::Operation text;
        TxCoordinator::Completion pending;
        f.backend->cwTextQueueWriter = [&](bool, const auto& operation, const auto& completion) {
            text = operation;
            pending = completion;
        };
        const TxCoordinator::Request cw = f.radio.registerTxProducer().request();
        check(f.radio.requestProducerCwx(cw, QStringLiteral("CQ")), "independent CW text queues over manual MOX");
        f.commands.clear();
        if (scoped) {
            hold.stop();
        } else {
            f.radio.setTransmit(false);
        }
        check(f.commands.contains("mox:off") && text.permitsDispatch(TxCoordinator::monotonicMs()),
              "pending CW text cannot swallow manual MOX release or lose its own queue authority");
        pending.finish();
        check(!text.permitsDispatch(TxCoordinator::monotonicMs()), "CW handoff drains without leaving a manual PTT intent");
    }
    Fixture f;
    f.radio.setTransmit(true);
    const auto controller = std::make_shared<TxController>(&f.radio);
    const TxController::Input hold = controller->capture(TxController::Activity::Mox);
    check(hold.start(), "scoped MOX joins legacy operator MOX");
    f.commands.clear();
    f.radio.setTransmit(false);
    check(f.commands.isEmpty() && hold.valid(), "ordinary legacy MOX release preserves another producer's hold");
    hold.stop();
    check(f.commands.contains("mox:off"), "last scoped MOX release reaches the backend");
}

void protocolCwxLifetimes()
{
    const auto drain = [] {
        for (int i = 0; i != 6; ++i) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        }
    };
    QStringList wire;
    RadioConnection connection;
    Fixture f;
    TxOperationIntegrationTestAccess::injectTcp(f.radio, connection, wire);
    {
        RigctlProtocol abandoned(&f.radio);
        check(abandoned.handleLine("b CQ") == "RPRT 0\n", "rigctl queues syntactically valid scoped Morse input");
    }
    drain();
    check(wire.filter("cwx send").isEmpty(), "CAT lifetime ends before queued Morse can be admitted");
    {
        SmartCatProtocol owner(&f.radio);
        RigctlProtocol unrelated(&f.radio);
        (void)owner.processCommand("KY CQ");
        drain();
        check(wire.filter("cwx send").size() == 1, "SmartCAT sends through the production scoped CW route");
        wire.clear();
        (void)unrelated.handleLine("\\stop_morse");
        drain();
        check(wire.isEmpty(), "another CAT client's stop_morse cannot clear the active producer's queue");
        owner.releasePtt();
        drain();
        check(wire.contains("cwx clear"), "SmartCAT disconnect aborts only its original CW queue");
    }
    drain();
}

void producerTuneAndAtu()
{
    {
        Fixture f;
        const TxCoordinator::Request request = f.radio.registerTxProducer().request();
        const TxCoordinator::Request competitor = f.radio.registerTxProducer().request();
        QObject::connect(&f.radio.transmitModel(), &TransmitModel::tuneChanged, &f.radio, [&](bool on) {
            if (on) {
                f.radio.requestProducerTune(competitor, true);
                f.radio.requestProducerTune(competitor, false);
            }
        });
        check(f.radio.requestProducerTune(request, true) && f.commands.contains("tune:on"),
              "a reentrant refused producer cannot invalidate another producer's TUNE start");
        f.radio.requestProducerTune(request, false);
    }
    for (const bool tuner : {false, true}) {
        Fixture f;
        const TxCoordinator::Producer producer = f.radio.registerTxProducer();
        const TxCoordinator::Request request = producer.request();
        TxCoordinator::Operation queued;
        TxCoordinator::Completion completion;
        const RecordingBackend::Writer writer = [&](bool on, const auto& operation, const auto& done) {
            if (on) {
                queued = operation;
            } else {
                completion = done;
            }
        };
        f.backend->tuneWriter = writer;
        f.backend->atuWriter = writer;
        const auto drive = [&](bool on) {
            return tuner ? f.radio.requestProducerAtu(request, on)
                         : f.radio.requestProducerTune(request, on, true);
        };
        check(drive(true) && queued.permitsDispatch(TxCoordinator::monotonicMs()),
              "producer TUNE/ATU uses the typed engine route and original request");
        const qsizetype before = f.commands.size();
        const TxCoordinator::Request competitor = f.radio.registerTxProducer().request();
        check(!(tuner ? f.radio.requestProducerAtu(competitor, true)
                      : f.radio.requestProducerTune(competitor, true))
                  && !competitor.valid() && f.commands.size() == before,
              "another producer cannot replace a live singleton TUNE/ATU context");
        f.radio.setProducerTransmit(request, false);
        f.radio.abortProducerPtt(request, TransmitModel::PttSource::Mox);
        check(f.commands.size() == before && request.valid(),
              "a PTT release cannot consume a different activity's request");
        check(drive(false) && queued.permitsDispatch(TxCoordinator::monotonicMs()),
              "scoped TUNE/ATU keeps a short queued pulse alive through its own cleanup");
        const qsizetype released = f.commands.size();
        drive(false);
        check(f.commands.size() == released, "duplicate scoped TUNE/ATU release cannot write twice");
        completion.finish();
        check(!queued.permitsDispatch(TxCoordinator::monotonicMs()),
              "TUNE/ATU queue consumption ends only its original contribution");
    }
    {
        Fixture f;
        const TxCoordinator::Request request = f.radio.registerTxProducer().request();
        f.radio.requestProducerAtu(request, true);
        const TxCoordinator::Operation operation = f.radio.transmitOperation();
        TransmitDelta delta;
        delta.atuStatusRaw = QStringLiteral("TUNE_SUCCESSFUL");
        TxOperationIntegrationTestAccess::transmitDelta(f.radio, delta);
        check(!operation.permitsDispatch(TxCoordinator::monotonicMs()),
              "ATU terminal readback retires its original scoped contribution");
    }
    {
        Fixture f;
        const TxCoordinator::Request request = f.radio.registerTxProducer().request();
        f.radio.transmitModel().setPttPreflight([](TransmitModel::PttSource) {
            return QStringLiteral("test refusal");
        });
        check(!f.radio.requestProducerTune(request, true) && !request.valid() && f.commands.isEmpty(),
              "scoped TUNE preflight refusal closes input without dispatching or changing authority");
    }
}

#ifdef HAVE_WEBSOCKETS
void tciProducerLifetimes()
{
    // Drive the production server handler with disconnected WebSocket
    // objects. No listen(), connect(), socket peer, or radio transport.
    Fixture f;
    QWebSocket socket;
    TciServer server(&f.radio);
    TxOperationIntegrationTestAccess::addTciClient(server, socket, f.radio);
    TxOperationIntegrationTestAccess::deferTciRoute(server);
    TxOperationIntegrationTestAccess::tciRequest(server, socket, true);
    TxOperationIntegrationTestAccess::tciRequest(server, socket, false);
    TxOperationIntegrationTestAccess::drainTciRoute(server);
    check(f.commands.isEmpty(), "TCI off before deferred route completion cannot key later");
    TxOperationIntegrationTestAccess::tciRequest(server, socket, true);
    check(f.commands.contains("mox:on"), "fresh TCI request is admitted through its accepted session");
    const TxCoordinator::Producer other = f.radio.registerTxProducer();
    const TxCoordinator::Request otherRequest = other.request();
    check(f.radio.setProducerTransmit(otherRequest, true), "CAT-compatible contributor can join TCI operation");
    f.commands.clear();
    TxOperationIntegrationTestAccess::disconnectTciClient(server, socket);
    check(f.commands.isEmpty() && f.radio.captureTxMedia(otherRequest).permitsDispatch(TxCoordinator::monotonicMs()),
          "TCI teardown retains another producer's PTT and media authority");
    f.radio.setProducerTransmit(otherRequest, false);
}
#endif
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("tx-operation-integration"));
    if (!settings.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    primaryRoutes();
    perIntentCompletion();
    overlappingCwContributions();
    disconnectDuringEnteredWrite();
    refusedStartsAndUnconditionalStops();
    localCompletionDoesNotAuthorizeHandoff();
    cwTuneMutualExclusion();
    delayedReleaseAndReplacement();
    flexEncoding();
    queuedPrimaryKeying();
    teardownAdmission();
    disconnectAdmission();
    reentrantIntents();
    quindarNormalRelease();
    cwxCancellationFence();
    queuedCwxCancellation();
    cwxCompletionAndRefusal();
    cwxFailureAndSpeedRestore();
    flexCwxLifecycle();
    queuedNetCwEdges();
    queuedCwSessionAndTcpFences();
    scopedCompatibilityStop();
    operatorCancelsCapturedInputs();
    derivedInputsStayWithTheirProducer();
    queuedDerivedCwElements();
    deviceCloseFencesOriginalInputs();
    compoundAndDeviceControllerScopes();
    scopedWsprRoutes();
    cwxCallbackLifetimes();
    serialInputQueueLifetimes();
    testInjectionReopensAdmission();
    protocolProducerLifetimes();
    producerNormalTails();
    producerTuneAndAtu();
    producerCwxQueue();
    nativeCwxUsesOperatorProducer();
    cwQueueDoesNotOwnManualPtt();
    protocolCwxLifetimes();
#ifdef HAVE_WEBSOCKETS
    tciProducerLifetimes();
#endif
    return failures ? 1 : 0;
}
