// Production encoding and model-to-terminal dispatch, with no socket peer or RF.
#include "TestSettingsProfile.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/TciProtocol.h"
#include "models/RadioModel.h"
#include "models/TxController.h"
#include <QCoreApplication>
#include <QEvent>
#include <QThread>
#include <cstdio>
#include <memory>
#include <utility>

namespace AetherSDR {
class TxOperationIntegrationTestAccess {
public:
    static void inject(RadioModel& radio, RadioConnection& connection, FlexBackend& backend, QStringList& wire)
    {
        connection.m_commandSinkForTest = [&wire](quint32, const QString& command) { wire << command; };
        radio.m_connection = &connection;
        radio.wireFlexTransmitServices(&backend);
    }
    static void retire(RadioModel& radio) { radio.resetTxOperations(); }
    static void reply(RadioModel& radio, int code, const QString& body = {})
    {
        const auto pending = std::exchange(radio.m_pendingCallbacks, {});
        for (const auto& callback : pending) { callback(code, body); }
    }
    static bool serviceHeld(RadioModel& radio)
    {
        return radio.m_dvkTxIntent.pending() || radio.m_navtexTxIntent.pending();
    }
};
}
using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool value, const char* label)
{
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    failures += !value;
}
void flush()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
}
class ConnectedFlex final : public FlexBackend {
public:
    bool connected = false;
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps = FlexBackend::capabilities();
        caps.receiveOnlyModes = {QStringLiteral("WFM")};
        return caps;
    }
    bool isConnected() const override { return connected; }
};
struct Fixture {
    QStringList wire;
    RadioConnection inert;
    RadioModel radio;
    Fixture()
    {
        auto backend = std::make_unique<ConnectedFlex>();
        ConnectedFlex* pointer = backend.get();
        radio.setBackendForTest(std::move(backend), "flex");
        TxOperationIntegrationTestAccess::inject(radio, inert, *pointer, wire);
        if (!radio.automationApplySliceFixture(0, "A") || !radio.slice(0)) {
            qFatal("Could not install the disconnected slice fixture");
        }
        SliceDelta delta;
        delta.txSlice = true;
        delta.frequency = 7.244;
        delta.mode = QStringLiteral("USB");
        radio.slice(0)->applyChanges(delta);
        pointer->connected = true;
        radio.dvkModel().applyStatus("dvk", {{"status", "idle"}, {"enabled", "1"}});
        flush();
        wire.clear();
    }
};
void encoding()
{
    FlexBackend backend;
    QStringList wire;
    backend.setServiceCommandSink([&](const QString& command, const RadioControlFence&, FlexBackend::CommandReply) {
        wire << command;
        return true;
    });
    backend.setRepliedTxSink([&](const QString& command, const TxCoordinator::Command&, FlexBackend::CommandReply) {
        wire << command;
        return true;
    });
    using Operation = DvkRequest::Operation;
    const QList<DvkRequest> requests{
        {Operation::RecordStart, 3, {}}, {Operation::RecordStop, 3, {}},
        {Operation::PreviewStart, 4, {}}, {Operation::PreviewStop, 4, {}},
        {Operation::PlaybackStart, 5, {}}, {Operation::PlaybackStop, 5, {}},
        {Operation::Clear, 6, {}}, {Operation::Remove, 7, {}},
        {Operation::Name, 8, "CQ Test"}
    };
    for (const DvkRequest& request : requests) { backend.requestDvk(request, {}); }
    check(wire == QStringList{"dvk rec_start id=3", "dvk rec_stop", "dvk preview_start id=4",
        "dvk preview_stop", "dvk playback_start id=5", "dvk playback_stop", "dvk clear id=6",
        "dvk remove id=7", "dvk set_name name=\"CQ Test\" id=8"}, "DVK encoding preserves global stop and slot verbs");
    wire.clear();
    backend.requestNavtex({'A', 'B', "say \"hi\" \\ test", 999}, {}, {});
    check(wire == QStringList{"navtex send tx_ident=A subject_indicator=B serial_num=999 msg_text=\"say \\\"hi\\\" \\\\ test\""},
          "NAVTEX backend escapes quotes and backslashes exactly once");
    wire.clear();
    backend.requestDvk({Operation::Name, 1, "bad\"\nxmit 1"}, {});
    backend.requestDvk({Operation::RecordStart, 0, {}}, {});
    backend.requestNavtex({'A', 'B', "bad\r\nxmit 1", {}}, {}, {});
    check(wire.isEmpty(), "backend revalidates hostile fields even without model validation");
    backend.setRepliedTxSink({});
    backend.setCommandSink([&](const QString& command) { wire << command; });
    check(backend.requestDvk({Operation::PlaybackStart, 1, {}}, {}) == ReceiveDispatch::Unsupported
        && backend.requestNavtex({'A', 'B', "test", {}}, {}, {}) == ReceiveDispatch::Unsupported
        && wire.isEmpty(), "keying operations cannot fall back to an unfenced generic writer");
}
void lifecycle()
{
    DvkModel model;
    quint64 latest = 0;
    int requests = 0;
    QObject::connect(&model, &DvkModel::controlRequested, &model,
        [&](const DvkRequest&, quint64 token) { latest = token; ++requests; });
    model.previewStart(1);
    const quint64 old = latest;
    model.reset();
    model.previewStart(2);
    model.handleControlReply(old, {TransmitServiceReply::Outcome::Rejected, 0x50004001, {}});
    check(model.pendingOperation() == VoiceKeyer::Preview && !model.licenseRefused(),
          "old-session rejection cannot clear a newer start or change licensing");
    model.handleControlReply(latest, {TransmitServiceReply::Outcome::Unknown, 0, {}});
    model.previewStart(3);
    check(requests == 2 && !model.canStartOperation(), "unknown completion cannot admit an automatic retry");
    model.applyStatus("dvk", {{"status", "preview"}, {"id", "2"}});
    model.applyStatus("dvk", {{"status", "idle"}});
    check(model.canStartOperation(), "observed operation and idle settle an unknown reply");

    DvkModel saturated;
    QList<DvkRequest> queued;
    QObject::connect(&saturated, &DvkModel::controlRequested, &saturated,
        [&](const DvkRequest& request, quint64) { queued.append(request); });
    saturated.playbackStart(1);
    for (int i = 0; i < 40; ++i) { saturated.setName(2, "pending name"); }
    check(queued.size() == 29, "ordinary pending requests preserve cleanup capacity");
    saturated.playbackStop(1);
    check(queued.size() == 30 && queued.last().operation == DvkRequest::Operation::PlaybackStop,
          "STOP remains dispatchable when ordinary requests saturate the queue");
    saturated.recStop(1);
    saturated.previewStop(1);
    for (int i = 0; i < 40; ++i) { saturated.playbackStop(1); }
    check(queued.size() == 32, "all stop verbs remain available without unbounded duplicate requests");

    DvkModel advancing;
    int stops = 0;
    QObject::connect(&advancing, &DvkModel::controlRequested, &advancing,
        [&](const DvkRequest& request, quint64) { stops += request.stops(); });
    advancing.playbackStart(1);
    advancing.applyStatus("dvk", {{"status", "playback"}, {"id", "1"}});
    advancing.playbackStop(1);
    advancing.applyStatus("dvk", {{"status", "idle"}});
    advancing.playbackStart(2);
    advancing.playbackStop(2);
    check(stops == 2, "unanswered old STOP cannot suppress STOP for a newer playback");
}
void terminalFence()
{
    {
        Fixture f;
        const auto controller = f.radio.localTxController();
        for (const auto activity : {TxCoordinator::Activity::VoiceKeyer, TxCoordinator::Activity::Navtex}) {
            check(!controller->capture(activity).valid() && !controller->current(activity).valid()
                && !controller->captureProgram(activity).valid(),
                  "service activities cannot enter the desktop controller's fixed input table");
        }
    }
    {
        Fixture f;
        TransmitDelta vox;
        vox.voxEnable = true;
        f.radio.transmitModel().applyChanges(vox);
        f.radio.dvkModel().recStart(1);
        flush();
        check(f.wire.isEmpty(), "VOX-on refuses radio recording before any command");
        vox.voxEnable = false;
        f.radio.transmitModel().applyChanges(vox);
        f.radio.dvkModel().recStart(1);
        flush();
        check(f.wire == QStringList{"dvk rec_start id=1"}, "confirmed VOX-off admits recording intent");
    }
    {
        Fixture f;
        SliceDelta receiveOnly;
        receiveOnly.mode = QStringLiteral("WFM");
        f.radio.slice(0)->applyChanges(receiveOnly);
        f.radio.dvkModel().playbackStart(1);
        flush();
        check(!f.wire.contains("dvk playback_start id=1"), "receive-only mode refuses voice playback");
    }
    {
        Fixture f;
        f.radio.dvkModel().playbackStart(1);
        flush();
        check(f.wire.contains("dvk playback_start id=1") && TxOperationIntegrationTestAccess::serviceHeld(f.radio),
              "admitted voice playback reaches the injected terminal writer");
        TxOperationIntegrationTestAccess::reply(f.radio, 0);
        check(TxOperationIntegrationTestAccess::serviceHeld(f.radio), "acceptance retains playback intent");
        f.radio.dvkModel().applyStatus("dvk", {{"status", "playback"}, {"id", "1"}});
        f.radio.dvkModel().applyStatus("dvk", {{"status", "idle"}});
        check(!TxOperationIntegrationTestAccess::serviceHeld(f.radio), "observed playback completion retires its intent");
    }
    {
        Fixture f;
        f.radio.dvkModel().playbackStart(1);
        TxOperationIntegrationTestAccess::retire(f.radio);
        flush();
        check(!f.wire.contains("dvk playback_start id=1"), "retirement cancels playback already queued for the worker");
    }
    {
        Fixture f;
        f.radio.navtexModel().parseStatus("navtex", {{"status", "active"}});
        f.radio.navtexModel().sendMessage('A', 'B', "test");
        flush();
        check(f.wire == QStringList{"navtex send tx_ident=A subject_indicator=B msg_text=\"test\""},
              "admitted NAVTEX reaches the final injected writer");
        TxOperationIntegrationTestAccess::reply(f.radio, 0, "7");
        check(TxOperationIntegrationTestAccess::serviceHeld(f.radio), "queued NAVTEX retains TX intent");
        f.radio.navtexModel().parseStatus("navtex sent", {{"idx", "7"}, {"serial_num", "1"}});
        check(!TxOperationIntegrationTestAccess::serviceHeld(f.radio), "sent confirmation retires matching NAVTEX intent");
    }
    {
        Fixture f;
        f.radio.navtexModel().parseStatus("navtex", {{"status", "active"}});
        f.radio.navtexModel().sendMessage('A', 'B', "test");
        TxOperationIntegrationTestAccess::retire(f.radio);
        flush();
        check(f.wire.isEmpty(), "retirement cancels queued NAVTEX without inventing a radio cancel verb");
    }
    {
        Fixture f;
        QThread writer;
        f.inert.moveToThread(&writer);
        f.radio.dvkModel().setName(1, "queued name");
        TxOperationIntegrationTestAccess::retire(f.radio);
        writer.start();
        QMetaObject::invokeMethod(&f.inert, [&f] {
            f.inert.moveToThread(QCoreApplication::instance()->thread());
        }, Qt::BlockingQueuedConnection);
        writer.quit();
        writer.wait();
        flush();
        check(f.wire.isEmpty(), "non-keying DVK requests also respect session retirement at the writer");
    }
}
void tciUnsupported()
{
#ifdef HAVE_WEBSOCKETS
    Fixture f;
    TciProtocol tci(&f.radio);
    for (const QString& command : {QString("cw_macros_delay"), QString("cw_macros_delay:100"),
                                   QString("cw_macros_delay:0,100"), QString("cw_macros_delay:bad")}) {
        check(tci.handleCommand(command).isEmpty() && tci.pendingNotification().isEmpty(),
              "unsupported macro delay has no fabricated value or success echo");
    }
    flush();
    check(f.wire.isEmpty(), "TCI macro delay never changes radio break-in delay");
#endif
}
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile("transmit-services");
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    encoding();
    lifecycle();
    terminalFence();
    tciUnsupported();
    return failures ? 1 : 0;
}
