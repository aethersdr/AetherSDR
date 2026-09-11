// Socket-free production-path tests. Only an injected transport recorder is
// used; no radio, peer, listener, discovery or transmitter is opened.
#include "TestSettingsProfile.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/ClientQuindarTone.h"
#include "core/PanadapterStream.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <memory>
#include <limits>

using namespace AetherSDR;

namespace AetherSDR {
class TxOperationIntegrationTestAccess {
public:
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
    QString sendCwText(const QString& text) override { *commands << "cwx:" + text; return {}; }
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
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("tx-operation-integration"));
    if (!settings.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    primaryRoutes();
    refusedStartsAndUnconditionalStops();
    delayedReleaseAndReplacement();
    flexEncoding();
    teardownAdmission();
    disconnectAdmission();
    reentrantIntents();
    quindarNormalRelease();
    cwxCancellationFence();
    queuedNetCwEdges();
    return failures ? 1 : 0;
}
