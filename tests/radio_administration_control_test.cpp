// Production dispatch and inert transports; no sockets, firmware peer or RF.
#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "core/backends/flex/RadioConnection.h"
#include "core/backends/flex/WanConnection.h"
#include "core/backends/sim/SimBackend.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTest>
#include <cstdio>
#include <memory>
#include <utility>

namespace AetherSDR {
class RadioAdministrationTestAccess {
public:
    static void transport(RadioModel& radio, RadioConnection* connection) { radio.m_connection = connection; }
    static void connectInert(RadioConnection& connection,
        const std::function<void(quint32, const QString&)>& record)
    {
        connection.m_state.store(ConnectionState::Connected);
        connection.m_commandSinkForTest = record;
    }
    static void nextTransport(RadioConnection& connection) { ++connection.m_sessionGeneration; }
    static bool dispatch(RadioModel& radio, const QString& command, const RadioControlCompletion& reply,
        const RadioControlFence& current, const std::function<void()>& written)
    { return radio.dispatchAdministrationCommand(command, reply, current, written); }
    static void reply(RadioModel& radio, quint32 sequence, int code = 0)
    {
        const auto callback = radio.m_pendingCallbacks.value(sequence);
        if (callback) { callback(code, code ? QStringLiteral("rejected") : QString()); }
    }
    static void observe(RadioModel& radio, bool remote, bool multi)
    {
        RadioDelta delta;
        delta.remoteOnEnabled = remote;
        delta.multiFlexEnabled = multi;
        radio.applyRadioChanges(delta);
    }
    static void license(RadioModel& radio, const QString& value)
    { radio.m_licenseSubscription = value; emit radio.infoChanged(); }
    static void own(RadioModel& radio, int id, bool owned)
    {
        if (owned) { radio.m_ownedSliceIds.insert(id); }
        else { radio.m_ownedSliceIds.remove(id); }
        emit radio.slotOccupancyChanged(id);
    }
    static void retire(RadioModel& radio)
    { ++radio.m_sessionGeneration; radio.resetAdministration(); }
    static int pending(const RadioModel& radio) { return radio.m_administrationWrites.size(); }
    static void receipt(RadioModel& radio, quint64 ticket, bool written)
    { radio.administrationWriteResult(ticket, written); }
    static bool rebooting(const RadioModel& radio) { return radio.m_rebootInProgress; }
    static void expire(RadioModel& radio)
    {
        const auto tickets = radio.m_administrationWrites.keys();
        for (quint64 ticket : tickets) {
            radio.finishAdministrationWrite(ticket,
                {RadioControlReply::Kind::Failed, -1, QStringLiteral("outcome unknown")});
        }
    }
};
}

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool value, const char* description)
{
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", description);
    failures += !value;
}
void drain()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}
struct Fixture {
    RadioModel radio;
    RadioConnection connection;
    RadioSetupTestBackend* backend{};
    QStringList writes;
    QList<quint32> sequences;
    Fixture()
    {
        backend = installRadioSetupBackend(radio);
        RadioAdministrationTestAccess::connectInert(connection, [this](quint32 sequence, const QString& command) {
            sequences.append(sequence);
            writes.append(command);
        });
        RadioAdministrationTestAccess::transport(radio, &connection);
        backend->setAdministrationCommandSink([this](const QString& command,
            const RadioControlCompletion& reply, const RadioControlFence& current,
            const std::function<void()>& written) {
            return RadioAdministrationTestAccess::dispatch(radio, command, reply, current, written);
        });
        QObject::connect(&connection, &RadioConnection::administrationCommandWritten, &radio,
            [this](quint64 ticket, bool written) { RadioAdministrationTestAccess::receipt(radio, ticket, written); });
    }
    ~Fixture()
    {
        RadioAdministrationTestAccess::retire(radio);
        RadioAdministrationTestAccess::transport(radio, nullptr);
    }
    void settings(bool remote = false, bool multi = false)
    { RadioAdministrationTestAccess::observe(radio, remote, multi); }
    SliceModel* cw(int id = 0)
    {
        SliceModel* slice = RadioSetupControlTestAccess::addSlice(radio, id);
        SliceDelta delta;
        delta.mode = QStringLiteral("CW");
        slice->applyChanges(delta);
        RadioAdministrationTestAccess::own(radio, id, true);
        RadioAdministrationTestAccess::license(radio, QStringLiteral("SmartSDR+"));
        return slice;
    }
    void ack(int code = 0) { RadioAdministrationTestAccess::reply(radio, sequences.last(), code); }
};
}

namespace {
void settingsAndStaleWrites()
{
    Fixture fixture;
    check(!fixture.radio.administrationSettingAvailable(RadioSetupField::RemoteOnEnabled),
        "unobserved settings remain unavailable");
    fixture.radio.setRemoteOnEnabled(true);
    drain();
    check(fixture.writes.isEmpty(), "unknown initial state cannot write");
    fixture.settings();
    fixture.radio.setRemoteOnEnabled(true);
    check(!fixture.radio.remoteOnEnabled() && fixture.radio.remoteOnRequested(),
        "pending intent does not fabricate radio-confirmed state");
    fixture.radio.setRemoteOnEnabled(false);
    drain();
    check(fixture.writes == QStringList{"radio set remote_on_enabled=0"},
        "superseded queued setting cannot reach the transport");
    fixture.ack();
    check(!fixture.radio.remoteOnEnabled(), "ACK alone does not mutate confirmed state");
    fixture.settings(true, false);
    check(fixture.radio.remoteOnEnabled() && fixture.radio.remoteOnRequested(),
        "independent status publication is authoritative");
    fixture.radio.setRemoteOnEnabled(false);
    drain();
    const quint32 first = fixture.sequences.last();
    fixture.radio.setRemoteOnEnabled(true);
    RadioAdministrationTestAccess::reply(fixture.radio, first, 1);
    check(fixture.radio.remoteOnRequested(), "old failure cannot roll back newer intent");
    drain();
    fixture.ack(1);
    check(fixture.radio.remoteOnRequested(), "current refusal restores last confirmed value");
    fixture.radio.setMultiFlexEnabled(true);
    drain();
    check(fixture.writes.last() == QStringLiteral("radio set mf_enable=1")
        && !fixture.radio.multiFlexEnabled(), "multiFLEX uses its typed independent setting");
    RadioAdministrationTestAccess::expire(fixture.radio);
    check(!fixture.radio.multiFlexRequested() && RadioAdministrationTestAccess::pending(fixture.radio) == 0,
        "timeout retires pending state without retry");
}

void lifetimeFences()
{
    Fixture fixture;
    fixture.settings();
    int callbacks = 0;
    {
        QObject owner;
        fixture.radio.requestRadioSetup({RadioSetupField::RemoteOnEnabled, true}, &owner,
            [&](const RadioControlReply&) { ++callbacks; });
    }
    drain();
    check(fixture.writes.isEmpty() && callbacks == 0,
        "destroyed dialog cancels queued write and suppresses completion");
    fixture.radio.setRemoteOnEnabled(true);
    RadioAdministrationTestAccess::nextTransport(fixture.connection);
    drain();
    check(fixture.writes.isEmpty(), "replacement wire session rejects old queued write");
    RadioAdministrationTestAccess::expire(fixture.radio);
    fixture.radio.setRemoteOnEnabled(true);
    RadioAdministrationTestAccess::retire(fixture.radio);
    drain();
    check(fixture.writes.isEmpty() && !fixture.radio.administrationSettingAvailable(RadioSetupField::RemoteOnEnabled),
        "model retirement cancels writes and requires fresh setting observations");

    RadioConnection inert;
    int writes = 0;
    RadioAdministrationTestAccess::connectInert(inert, [&](quint32, const QString&) { ++writes; });
    const auto guard = SessionCommandGuard::create().request();
    const quint64 generation = inert.commandSessionGeneration();
    check(inert.writeAdministrationCommand(1, "radio reboot", generation, guard) && writes == 1,
        "current terminal writer accepts a live session");
    guard.cancel();
    check(!inert.writeAdministrationCommand(2, "radio reboot", generation, guard) && writes == 1,
        "cancelled atomic guard refuses the terminal write");
    const auto expired = SessionCommandGuard::create().request(0);
    check(!inert.writeAdministrationCommand(3, "radio reboot", generation, expired),
        "expired guard refuses the terminal write");
    WanConnection wan;
    check(wan.sendAdministrationCommand("radio reboot", wan.commandSessionGeneration(),
        SessionCommandGuard::create().request(), {}) == 0,
        "disconnected WAN never opens a socket to service administration");
}

void cwOwnershipAndOrdering()
{
    Fixture fixture;
    SliceModel* slice = fixture.cw();
    fixture.radio.requestCwAutoTune(slice, CwAutoTuneAction::Once);
    drain();
    check(fixture.writes == QStringList{"slice auto_tune 0"}, "Once uses the SDK nullable-bool form");
    fixture.ack();
    fixture.radio.requestCwAutoTune(slice, CwAutoTuneAction::Start);
    check(fixture.radio.cwAutoTuneLoopRequested(0), "loop intent belongs to model, independent of widget lifetime");
    fixture.radio.requestCwAutoTune(slice, CwAutoTuneAction::Stop);
    drain();
    check(fixture.writes == QStringList{"slice auto_tune 0", "slice auto_tune 0 int=0"}
        && !fixture.radio.cwAutoTuneLoopRequested(0), "Stop invalidates an unsent Start without changing slice target");
    fixture.ack();
    fixture.radio.requestCwAutoTune(slice, CwAutoTuneAction::Start);
    drain();
    fixture.ack();
    SliceDelta locked;
    locked.locked = true;
    slice->applyChanges(locked);
    drain();
    check(fixture.writes.last() == QStringLiteral("slice auto_tune 0 int=0")
        && !fixture.radio.cwAutoTuneLoopRequested(0), "locking an active loop permits Stop cleanup");
    fixture.ack();
    const qsizetype count = fixture.writes.size();
    fixture.radio.requestCwAutoTune(slice, CwAutoTuneAction::Once);
    fixture.radio.requestCwAutoTune(slice, CwAutoTuneAction::Start);
    drain();
    check(fixture.writes.size() == count, "locked slice refuses both start forms");
    locked.locked = false;
    slice->applyChanges(locked);
    fixture.radio.requestCwAutoTune(slice, CwAutoTuneAction::Start);
    drain();
    fixture.ack();
    RadioAdministrationTestAccess::license(fixture.radio, {});
    drain();
    check(fixture.writes.last() == QStringLiteral("slice auto_tune 0 int=0"),
        "entitlement loss cleans up an existing loop without enabling new tuning");
    fixture.ack();
    RadioAdministrationTestAccess::license(fixture.radio, QStringLiteral("SmartSDR+"));
    fixture.radio.requestCwAutoTune(slice, CwAutoTuneAction::Start);
    RadioAdministrationTestAccess::own(fixture.radio, 0, false);
    drain();
    check(!fixture.radio.cwAutoTuneLoopRequested(0) && fixture.writes.last() == QStringLiteral("slice auto_tune 0 int=0"),
        "ownership loss cancels queued Start and never writes foreign cleanup");
    SliceModel* replacement = fixture.cw();
    fixture.radio.requestCwAutoTune(slice, CwAutoTuneAction::Once);
    fixture.radio.requestCwAutoTune(replacement, CwAutoTuneAction::Stop);
    drain();
    check(!fixture.radio.cwAutoTuneLoopRequested(0), "reused slice ID cannot inherit old loop or old widget intent");
}

void backendValidationAndReboot()
{
    RadioSetupTestBackend backend;
    const auto live = [] { return true; };
    check(backend.requestRadioReboot({}, live, {}) == ReceiveDispatch::Dispatched
        && backend.commands == QStringList{"radio reboot"}, "reboot has exact Flex encoding");
    backend.commands.clear();
    backend.requestCwAutoTune(-1, CwAutoTuneAction::Once, {}, live);
    backend.requestCwAutoTune(0, static_cast<CwAutoTuneAction>(90), {}, live);
    backend.requestCwAutoTune(0, CwAutoTuneAction::Once, {}, [] { return false; });
    backend.requestRadioSetup({RadioSetupField::RemoteOnEnabled, 1}, {}, live);
    backend.requestRadioSetup({RadioSetupField::MultiFlexEnabled, true}, {}, {});
    backend.requestRadioReboot({}, {}, {});
    check(backend.commands.isEmpty(), "invalid targets, values, actions and absent fences refuse before encoding");
    SimBackend demo;
    check(!demo.capabilities().cwReceiveControl
        && demo.requestCwAutoTune(0, CwAutoTuneAction::Start, {}, live) == ReceiveDispatch::Unsupported
        && demo.requestRadioReboot({}, live, {}) == ReceiveDispatch::Unsupported,
        "non-Flex defaults advertise no CW auto-tune or administration verb");
    Fixture fixture;
    const auto session = fixture.radio.controlSession();
    RadioAdministrationTestAccess::retire(fixture.radio);
    check(!fixture.radio.rebootRadio(session), "reboot confirmation cannot retarget a replacement session");
    check(fixture.radio.rebootRadio() && !fixture.radio.rebootRadio(), "duplicate reboot clicks dispatch only once");
    drain();
    check(fixture.writes == QStringList{"radio reboot"}, "model reboot reaches guarded writer exactly once");
    fixture.ack(1);
    check(!RadioAdministrationTestAccess::rebooting(fixture.radio), "explicit refusal cancels reboot recovery");
    QTest::qWait(300);
    check(fixture.backend->live, "refused reboot timer cannot disconnect the session");
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-administration-test"));
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) { return 1; }
    settingsAndStaleWrites();
    lifetimeFences();
    cwOwnershipAndOrdering();
    backendValidationAndReboot();
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
