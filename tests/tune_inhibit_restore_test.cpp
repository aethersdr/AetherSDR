// Inject the production command writer and status path; no socket, peer or RF.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/flex/FlexBackend.h"
#include "models/RadioModel.h"
#include <QCoreApplication>
#include <QEvent>
#include <cstdio>
#include <memory>
#include <utility>

namespace AetherSDR {
class TxOperationIntegrationTestAccess {
public:
    static void inject(RadioModel& radio, RadioConnection& connection, QStringList& wire)
    {
        connection.m_commandSinkForTest = [&wire](quint32, const QString& command) { wire << command; };
        radio.m_connection = &connection;
    }
    static bool apply(RadioModel& radio) { return radio.applyTuneInhibit(); }
    static void restore(RadioModel& radio) { radio.restoreTuneInhibit(); }
    static void disconnect(RadioModel& radio) { radio.onDisconnected(); }
    static void reopen(RadioModel& radio) { radio.m_txSessionClosing = false; }
    static void closing(RadioModel& radio, bool value) { radio.m_txSessionClosing = value; }
    static void expiring(RadioModel& radio, bool value) { radio.m_expiringPendingCallbacks = value; }
    static QList<RadioModel::ResponseCallback> callbacks(const RadioModel& radio)
    {
        return radio.m_pendingCallbacks.values();
    }
    static void replyAll(RadioModel& radio, int code)
    {
        const auto callbacks = std::exchange(radio.m_pendingCallbacks, {});
        for (const auto& callback : callbacks) { callback(code, {}); }
    }
    static void onWrite(RadioConnection& connection, std::function<void(const QString&)> callback)
    {
        connection.m_commandSinkForTest = [callback](quint32, const QString& command) { callback(command); };
    }
    static bool invalidStart(RadioModel& radio, bool atu)
    {
        return atu ? radio.dispatchAtuIntent(true) : radio.dispatchTuneIntent(true);
    }
};
}
using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}
void flush() { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }
void preferences(bool acc, bool tx1, bool tx2, bool tx3)
{
    AppSettings& settings = AppSettings::instance();
    const bool selected[] = {acc, tx1, tx2, tx3};
    const char* keys[] = {"TuneInhibitAccTx", "TuneInhibitTx1", "TuneInhibitTx2", "TuneInhibitTx3"};
    for (int i = 0; i < 4; ++i) {
        settings.setValue(QLatin1String(keys[i]), selected[i] ? "True" : "False");
    }
}
class ConnectedFlex final : public FlexBackend {
public:
    bool connected = false;
    bool isConnected() const override { return connected; }
};
struct Fixture {
    QStringList wire;
    RadioConnection inert;
    RadioModel radio;
    ConnectedFlex* backend;
    Fixture()
    {
        auto owned = std::make_unique<ConnectedFlex>();
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("flex"));
        TxOperationIntegrationTestAccess::inject(radio, inert, wire);
        check(radio.automationApplySliceFixture(0, QStringLiteral("A")), "injected TX slice installed");
        SliceDelta delta;
        delta.txSlice = true;
        delta.frequency = 14.1;
        radio.slice(0)->applyChanges(delta);
        backend->connected = true;
        band(9, "20", true, true, false, false);
        band(5, "40", true, true, true, true);
        flush();
        wire.clear();
    }
    void band(int id, const char* name, bool acc, bool tx1, bool tx2, bool tx3)
    {
        radio.handleStatusForTest(QStringLiteral("interlock band %1").arg(id), {
            {"band_name", QLatin1String(name)}, {"acc_tx_enabled", acc ? "1" : "0"},
            {"tx1_enabled", tx1 ? "1" : "0"}, {"tx2_enabled", tx2 ? "1" : "0"},
            {"tx3_enabled", tx3 ? "1" : "0"}});
    }
    void apply() { TxOperationIntegrationTestAccess::apply(radio); flush(); }
    void restore() { TxOperationIntegrationTestAccess::restore(radio); flush(); }
};
void mixedOutputs()
{
    preferences(false, true, true, true);
    Fixture f;
    f.apply();
    check(f.wire == QStringList{"interlock bandset 9 tx1_enabled=0"}, "apply disables only enabled TX1");
    f.restore();
    check(f.wire == QStringList{"interlock bandset 9 tx1_enabled=0", "interlock bandset 9 tx1_enabled=1"},
          "restore never enables TX2/TX3 that were already disabled");
}
void preferenceChanges()
{
    for (int output = 0; output < 4; ++output) {
        const char* fields[] = {"acc_tx_enabled", "tx1_enabled", "tx2_enabled", "tx3_enabled"};
        preferences(output == 0, output == 1, output == 2, output == 3);
        Fixture f;
        f.band(9, "20", true, true, true, true);
        f.apply();
        preferences(false, false, false, false);
        f.restore();
        const QString prefix = QStringLiteral("interlock bandset 9 %1=").arg(QLatin1String(fields[output]));
        check(f.wire == QStringList{prefix + "0", prefix + "1"},
              "unchecking an inhibit preference cannot strand its temporarily disabled output");
    }
    preferences(false, true, false, false);
    Fixture f;
    f.apply();
    preferences(true, true, true, true);
    f.restore();
    check(f.wire == QStringList{"interlock bandset 9 tx1_enabled=0", "interlock bandset 9 tx1_enabled=1"},
          "new preference selections cannot acquire restoration ownership mid-cycle");
}
void ownershipAndChurn()
{
    preferences(true, true, true, true);
    Fixture f;
    f.apply();
    f.band(9, "20", false, false, false, false);
    SliceDelta frequency;
    frequency.frequency = 7.1;
    f.radio.slice(0)->applyChanges(frequency);
    f.apply();
    f.restore();
    check(f.wire == QStringList{"interlock bandset 9 acc_tx_enabled=0", "interlock bandset 9 tx1_enabled=0",
                               "interlock bandset 9 acc_tx_enabled=1", "interlock bandset 9 tx1_enabled=1"},
          "repeat apply and TX band change retain the original band and exact output set");
    const QStringList once = f.wire;
    f.restore();
    check(f.wire == once, "repeat cleanup is idempotent");

    Fixture local;
    local.apply();
    local.radio.sendCommand("interlock bandset 9 acc_tx_enabled=0");
    local.radio.sendCmdPublic("interlock bandset 5 tx1_enabled=0", {});
    local.restore();
    check(local.wire.count("interlock bandset 9 acc_tx_enabled=1") == 0
        && local.wire.count("interlock bandset 9 tx1_enabled=1") == 1,
          "explicit same-value output edit wins while another band's edit cannot cancel ownership");

    Fixture rejected;
    rejected.apply();
    TxOperationIntegrationTestAccess::replyAll(rejected.radio, 0x5000002d);
    rejected.restore();
    check(rejected.wire == QStringList{"interlock bandset 9 acc_tx_enabled=0", "interlock bandset 9 tx1_enabled=0"},
          "rejected disable commands create no later enable writes");

    Fixture lost;
    lost.apply();
    lost.backend->connected = false;
    TxOperationIntegrationTestAccess::disconnect(lost.radio);
    lost.backend->connected = true;
    TxOperationIntegrationTestAccess::reopen(lost.radio);
    lost.wire.clear();
    lost.restore();
    lost.apply();
    lost.restore();
    check(lost.wire.isEmpty(), "disconnect discards restore ownership and old-session band observations");
    lost.radio.handleStatusForTest("interlock band 9", {{"acc_tx_enabled", "1"}});
    lost.apply();
    lost.restore();
    check(lost.wire == QStringList{"interlock bandset 9 acc_tx_enabled=0", "interlock bandset 9 acc_tx_enabled=1"},
          "partial fresh readback cannot revive stale outputs from another session");

    Fixture replaced;
    replaced.apply();
    replaced.radio.setBackendForTest(std::make_unique<ConnectedFlex>(), "flex");
    replaced.wire.clear();
    replaced.restore();
    check(replaced.wire.isEmpty(), "backend replacement cannot restore into its successor");

    Fixture denied;
    check(!TxOperationIntegrationTestAccess::invalidStart(denied.radio, false)
        && !TxOperationIntegrationTestAccess::invalidStart(denied.radio, true), "unadmitted TUNE and ATU dispatch are refused");
    flush();
    denied.restore();
    check(denied.wire.isEmpty(), "rejected starts never alter output ownership");
}
void reentrantPaths()
{
    preferences(true, true, true, true);
    Fixture f;
    bool once = false;
    TxOperationIntegrationTestAccess::onWrite(f.inert, [&](const QString& command) {
        f.wire << command;
        if (!std::exchange(once, true)) {
            f.radio.sendCommand("interlock bandset 9 tx1_enabled=0");
            TxOperationIntegrationTestAccess::apply(f.radio);
        }
    });
    f.apply();
    f.restore();
    check(f.wire == QStringList{"interlock bandset 9 acc_tx_enabled=0", "interlock bandset 9 tx1_enabled=0",
                               "interlock bandset 9 acc_tx_enabled=1"},
          "reentrant edit cancels an unsent inhibit candidate and repeat start cannot reacquire it");
    TxOperationIntegrationTestAccess::inject(f.radio, f.inert, f.wire);

    Fixture cleanup;
    cleanup.apply();
    cleanup.wire.clear();
    once = false;
    TxOperationIntegrationTestAccess::onWrite(cleanup.inert, [&](const QString& command) {
        cleanup.wire << command;
        if (!std::exchange(once, true)) {
            TxOperationIntegrationTestAccess::restore(cleanup.radio);
            cleanup.radio.sendCmdPublic("interlock bandset 9 tx1_enabled=0", {});
        }
    });
    cleanup.restore();
    check(cleanup.wire == QStringList{"interlock bandset 9 acc_tx_enabled=1", "interlock bandset 9 tx1_enabled=0"},
          "reentrant cleanup is idempotent and an output edit cancels a remaining restore");
    TxOperationIntegrationTestAccess::inject(cleanup.radio, cleanup.inert, cleanup.wire);

    Fixture interrupted;
    once = false;
    TxOperationIntegrationTestAccess::onWrite(interrupted.inert, [&](const QString& command) {
        interrupted.wire << command;
        if (!std::exchange(once, true)) {
            TxOperationIntegrationTestAccess::restore(interrupted.radio);
        }
    });
    interrupted.apply();
    check(interrupted.wire == QStringList{"interlock bandset 9 acc_tx_enabled=0", "interlock bandset 9 acc_tx_enabled=1"},
          "cleanup during apply restores only a sent disable and stops remaining candidates");
    TxOperationIntegrationTestAccess::inject(interrupted.radio, interrupted.inert, interrupted.wire);
}
void lateRepliesAndDrops()
{
    preferences(false, true, false, false);
    Fixture f;
    f.apply();
    const auto old = TxOperationIntegrationTestAccess::callbacks(f.radio);
    f.restore();
    f.apply();
    for (const auto& callback : old) { callback(0x5000002d, {}); }
    f.restore();
    check(f.wire.count("interlock bandset 9 tx1_enabled=1") == 2,
          "late rejection cannot remove a newer cycle's restoration ownership");
    Fixture drop;
    TxOperationIntegrationTestAccess::expiring(drop.radio, true);
    drop.apply();
    TxOperationIntegrationTestAccess::expiring(drop.radio, false);
    drop.restore();
    check(drop.wire.isEmpty(), "zero-return dispatch without a callback acquires no restoration write");
}
void allOutputCombinations()
{
    Fixture f;
    const char* fields[] = {"acc_tx_enabled", "tx1_enabled", "tx2_enabled", "tx3_enabled"};
    bool correct = true;
    for (int enabled = 0; enabled < 16; ++enabled) {
        for (int selected = 0; selected < 16; ++selected) {
            preferences(selected & 1, selected & 2, selected & 4, selected & 8);
            f.band(9, "20", enabled & 1, enabled & 2, enabled & 4, enabled & 8);
            f.wire.clear();
            f.apply();
            preferences(false, false, false, false);
            f.restore();
            QStringList expected;
            for (int value : {0, 1}) {
                for (int output = 0; output < 4; ++output) {
                    if (enabled & selected & (1 << output)) {
                        expected << QStringLiteral("interlock bandset 9 %1=%2").arg(QLatin1String(fields[output])).arg(value);
                    }
                }
            }
            correct &= f.wire == expected;
        }
    }
    check(correct, "all 256 enabled/selected output combinations restore exactly their intersection despite preference changes");
}
void reentrantStartDuringRestore()
{
    preferences(true, true, false, false);
    Fixture f;
    f.apply();
    f.band(9, "20", false, false, false, false);
    f.wire.clear();
    bool once = false;
    f.backend->setTxCommandSink([&](const QString& command, const TxCoordinator::Command&) { f.wire << command; });
    TxOperationIntegrationTestAccess::onWrite(f.inert, [&](const QString& command) {
        f.wire << command;
        if (!std::exchange(once, true)) { f.radio.transmitModel().startTune(); }
    });
    f.restore();
    check(!f.wire.contains("transmit tune 1") && !f.radio.transmitModel().isTuning()
        && f.wire.count("interlock bandset 9 acc_tx_enabled=1") == 1
        && f.wire.count("interlock bandset 9 tx1_enabled=1") == 1,
          "reentrant production start during restore is retired without losing remaining cleanup");
    TxOperationIntegrationTestAccess::inject(f.radio, f.inert, f.wire);
    f.radio.transmitModel().startTune();
    check(f.wire.contains("transmit tune 1") && f.radio.transmitModel().isTuning(),
          "the refused reentrant start leaves no intent blocking the next admitted request");
    f.radio.transmitModel().stopTune();
}

void refusalAndClosing()
{
    preferences(true, true, false, false);
    Fixture f;
    f.apply();
    f.wire.clear();
    bool once = false;
    f.backend->setTxCommandSink([&](const QString& command, const TxCoordinator::Command&) { f.wire << command; });
    TxOperationIntegrationTestAccess::onWrite(f.inert, [&](const QString& command) {
        f.wire << command;
        if (!std::exchange(once, true)) { f.radio.transmitModel().atuStart(); }
    });
    f.restore();
    check(!f.wire.contains("atu start") && f.wire.count("interlock bandset 9 tx1_enabled=1") == 1,
          "reentrant ATU start during restore is refused without losing cleanup");
    TxOperationIntegrationTestAccess::inject(f.radio, f.inert, f.wire);
    f.radio.transmitModel().startTune();
    check(f.wire.contains("transmit tune 1"), "refused ATU intent cannot block subsequent TUNE admission");
    f.radio.transmitModel().stopTune();

    Fixture closing;
    closing.apply();
    TxOperationIntegrationTestAccess::closing(closing.radio, true);
    check(!TxOperationIntegrationTestAccess::apply(closing.radio),
          "an active snapshot cannot bypass session-closing refusal");
    closing.backend->connected = false;
    TxOperationIntegrationTestAccess::closing(closing.radio, false);
    check(!TxOperationIntegrationTestAccess::apply(closing.radio),
          "an active snapshot cannot bypass disconnected refusal");

    Fixture lastWrite;
    preferences(false, true, false, false);
    TxOperationIntegrationTestAccess::onWrite(lastWrite.inert, [&](const QString& command) {
        lastWrite.wire << command;
        TxOperationIntegrationTestAccess::closing(lastWrite.radio, true);
    });
    check(!TxOperationIntegrationTestAccess::apply(lastWrite.radio),
          "closure during the last disable cannot qualify a later keying dispatch");
    TxOperationIntegrationTestAccess::inject(lastWrite.radio, lastWrite.inert, lastWrite.wire);
}

void noOwnershipConnectivity()
{
    preferences(true, true, true, true);
    Fixture noBand;
    SliceDelta frequency;
    frequency.frequency = 100.0;
    noBand.radio.slice(0)->applyChanges(frequency);
    noBand.backend->connected = false;
    check(TxOperationIntegrationTestAccess::apply(noBand.radio) && noBand.wire.isEmpty(),
          "no TX band leaves disconnected dispatch behavior unchanged");

    Fixture noSelection;
    preferences(false, false, false, false);
    noSelection.backend->connected = false;
    check(TxOperationIntegrationTestAccess::apply(noSelection.radio) && noSelection.wire.isEmpty(),
          "no selected outputs leaves disconnected dispatch behavior unchanged");

    Fixture noFreshReadback;
    preferences(true, true, true, true);
    noFreshReadback.backend->connected = false;
    TxOperationIntegrationTestAccess::disconnect(noFreshReadback.radio);
    TxOperationIntegrationTestAccess::reopen(noFreshReadback.radio);
    check(TxOperationIntegrationTestAccess::apply(noFreshReadback.radio) && noFreshReadback.wire.isEmpty(),
          "no fresh output observations acquires no disconnected ownership");

    Fixture needsWrites;
    needsWrites.backend->connected = false;
    check(!TxOperationIntegrationTestAccess::apply(needsWrites.radio) && needsWrites.wire.isEmpty(),
          "selected fresh outputs still refuse disconnected inhibit writes");
}

void replacementDuringCleanup()
{
    preferences(true, true, false, false);
    QStringList successorWire;
    RadioConnection successor;
    Fixture f;
    f.apply();
    f.wire.clear();
    TxOperationIntegrationTestAccess::onWrite(f.inert, [&](const QString& command) {
        f.wire << command;
        auto replacement = std::make_unique<ConnectedFlex>();
        replacement->connected = true;
        f.radio.setBackendForTest(std::move(replacement), "flex");
        TxOperationIntegrationTestAccess::inject(f.radio, successor, successorWire);
    });
    f.restore();
    check(f.wire == QStringList{"interlock bandset 9 acc_tx_enabled=1"} && successorWire.isEmpty(),
          "replacement during cleanup fences every remaining restore away from the new backend");
}

void reentrantStartDuringApply()
{
    for (bool atu : {false, true}) {
        preferences(true, true, false, false);
        Fixture f;
        bool once = false;
        f.backend->setTxCommandSink([&](const QString& command, const TxCoordinator::Command&) { f.wire << command; });
        TxOperationIntegrationTestAccess::onWrite(f.inert, [&](const QString& command) {
            f.wire << command;
            if (!std::exchange(once, true)) {
                if (atu) { f.radio.transmitModel().atuStart(); }
                else { f.radio.transmitModel().startTune(); }
            }
        });
        f.radio.transmitModel().startTune();
        flush();
        const qsizetype key = f.wire.indexOf("transmit tune 1");
        const bool atuRefused = !f.wire.contains("atu start");
        const qsizetype secondDisable = f.wire.indexOf("interlock bandset 9 tx1_enabled=0");
        check(atuRefused && (key < 0 || (secondDisable >= 0 && secondDisable < key)),
              "a reentrant carrier cannot dispatch before all selected outputs have been inhibited");
        TxOperationIntegrationTestAccess::inject(f.radio, f.inert, f.wire);
        f.radio.transmitModel().stopTune();
        f.radio.transmitModel().atuBypass();
        f.wire.clear();
        f.radio.transmitModel().startTune();
        check(f.wire.contains("transmit tune 1") && f.radio.transmitModel().isTuning(),
              "a refused start during apply cannot block a later admitted TUNE request");
        f.radio.transmitModel().stopTune();
    }
}

void admittedProductionPaths()
{
    preferences(false, true, true, true);
    Fixture f;
    f.backend->setTxCommandSink([&](const QString& command, const TxCoordinator::Command& fence) {
        Q_UNUSED(fence);
        f.wire << command;
    });
    f.radio.transmitModel().startTune();
    flush();
    check(f.wire.contains("interlock bandset 9 tx1_enabled=0") && f.wire.contains("transmit tune 1"),
          "admitted production TUNE reaches injected disable and keying writers without RF");
    f.radio.transmitModel().stopTune();
    flush();
    check(f.wire.count("interlock bandset 9 tx1_enabled=1") == 1
        && !f.wire.contains("interlock bandset 9 tx2_enabled=1")
        && !f.wire.contains("interlock bandset 9 tx3_enabled=1") && f.wire.contains("transmit tune 0"),
          "production TUNE stop restores only the owned output");
}

}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("tune-inhibit-restore"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    mixedOutputs();
    preferenceChanges();
    ownershipAndChurn();
    reentrantPaths();
    lateRepliesAndDrops();
    allOutputCombinations();
    reentrantStartDuringRestore();
    refusalAndClosing();
    noOwnershipConnectivity();
    replacementDuringCleanup();
    reentrantStartDuringApply();
    admittedProductionPaths();
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
