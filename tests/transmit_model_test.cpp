#include "models/TransmitModel.h"
#include "core/backends/TransmitDelta.h"
#include "core/ClientQuindarTone.h"

#include <QCoreApplication>
#include <QObject>
#include <QStringList>

#include <iostream>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

// aetherd RFC 2.3: TransmitModel::applyChanges takes a typed TransmitDelta (the
// Flex wire decode + compander/dexp aliasing live in FlexBackend::decode*Status,
// covered by aetherd_transmit_decode_test). This builds a delta from a setter.
template <class F>
TransmitDelta td(F&& build)
{
    TransmitDelta d;
    build(d);
    return d;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    TransmitModel tx;
    QStringList commands;
    QStringList blockedMessages;
    QObject::connect(&tx, &TransmitModel::commandReady,
                     [&commands](const QString& cmd) { commands.append(cmd); });
    QObject::connect(&tx, &TransmitModel::pttBlocked,
                     [&blockedMessages](const QString& message) { blockedMessages.append(message); });

    bool ok = true;

    // A backend's MOX delta is the radio's live PTT answer, not this client's
    // transmit intent. RadioModel publishes it on radioTransmittingChanged;
    // TransmitModel must retain the observation without opening local mic,
    // DAX, recorder or serial-PTT consumers through moxChanged.
    QList<bool> radioTxEdges;
    QList<bool> localMoxEdges;
    QObject::connect(&tx, &TransmitModel::transmittingChanged,
                     [&radioTxEdges](bool on) { radioTxEdges.append(on); });
    QObject::connect(&tx, &TransmitModel::moxChanged,
                     [&localMoxEdges](bool on) { localMoxEdges.append(on); });
    tx.applyChanges(td([](TransmitDelta& d) { d.mox = true; }));
    ok &= expect(tx.isMox() && !tx.isTransmitting(),
                 "radio-reported MOX is retained without claiming local TX intent");
    tx.applyChanges(td([](TransmitDelta& d) { d.mox = false; }));
    ok &= expect(radioTxEdges.isEmpty() && localMoxEdges.isEmpty(),
                 "radio-reported MOX emits no local transmit-ownership edges");

    // ...but it MUST raise stateChanged(), because that is the only edge a
    // presentation consumer gating on isMox() can subscribe to (#5306).
    // MainWindow's PA-current label reads isMox() and repaints on this signal;
    // wiring it to moxChanged instead produces a slot that never fires, and
    // the assertion above is precisely why. Guard both directions so a future
    // change that folds mox into a silent assign, or one that "fixes" this by
    // reopening moxChanged, fails here.
    int stateEdges = 0;
    QObject::connect(&tx, &TransmitModel::stateChanged,
                     [&stateEdges]() { ++stateEdges; });
    tx.applyChanges(td([](TransmitDelta& d) { d.mox = true; }));
    ok &= expect(stateEdges == 1 && tx.isMox(),
                 "radio-reported MOX raises stateChanged() so isMox() consumers repaint");
    tx.applyChanges(td([](TransmitDelta& d) { d.mox = true; }));
    ok &= expect(stateEdges == 1,
                 "an unchanged MOX value raises no redundant stateChanged()");
    tx.applyChanges(td([](TransmitDelta& d) { d.mox = false; }));
    ok &= expect(stateEdges == 2 && !tx.isMox(),
                 "the un-key edge repaints too, so the label cannot latch on stale current");
    ok &= expect(localMoxEdges.isEmpty(),
                 "and none of that reopened the local moxChanged path");

    // ---- forced mic selection is ADOPTED, never commanded --------------------
    //
    // On a radio whose input a client cannot choose (an Icom picks its own from
    // its MOD Input menu), the Phone applet collapses the source list to PC.
    // The model has to agree with what the screen shows — it did not, and
    // reported MIC through a whole session, which made radiocert warn that
    // transmit audio capture was not running on a radio where that is simply
    // not how audio gets there.
    //
    // But the fix must NOT go through setMicSelection(): that is the operator
    // intent path, and pushing a capability-forced value back out as intent is
    // how a capability turns into a command nobody issued (Principle II). So
    // this asserts the state changed AND that nothing went on the wire.
    commands.clear();
    ok &= expect(tx.micSelection() == QStringLiteral("MIC"),
                 "the model starts on MIC, as a Flex would report");
    ok &= expect(tx.applyMicSelectionState(QStringLiteral("PC")),
                 "adopting a forced selection reports that it changed something");
    ok &= expect(tx.micSelection() == QStringLiteral("PC"),
                 "and the model now agrees with the screen");
    ok &= expect(commands.isEmpty(),
                 "WITHOUT emitting a command — a forced value is not operator intent");

    ok &= expect(!tx.applyMicSelectionState(QStringLiteral("PC")),
                 "re-adopting the same value is a no-op");
    ok &= expect(!tx.applyMicSelectionState(QString()),
                 "and an empty selection is refused rather than stored");
    ok &= expect(tx.micSelection() == QStringLiteral("PC"),
                 "so the last good value survives");
    ok &= expect(commands.isEmpty(), "none of which touched the wire");

    // The operator's own path still commands, so adopting did not break it.
    tx.setMicSelection(QStringLiteral("MIC"));
    ok &= expect(!commands.isEmpty(),
                 "the OPERATOR choosing a source still emits a command");
    // Leave the recorder clean: the assertions below compare `commands`
    // EXACTLY, so anything left here fails a test that has nothing to do with
    // mic selection.
    commands.clear();

    // CW controls must adopt operator intent even when no radio-side status
    // echo exists (HL2/software keyer). Otherwise the shortcut toggles the
    // same stale value forever and the local iambic keyer never sees swap.
    int cwPhoneEdges = 0;
    QObject::connect(&tx, &TransmitModel::phoneStateChanged,
                     [&cwPhoneEdges] { ++cwPhoneEdges; });
    tx.setCwSwapPaddles(true);
    ok &= expect(tx.cwSwapPaddles()
                 && commands == QStringList({"cw swap 1"})
                 && cwPhoneEdges == 1,
                 "CW paddle swap is adopted locally and still commands Flex");
    commands.clear();
    tx.setCwlEnabled(true);
    ok &= expect(tx.cwlEnabled()
                 && commands == QStringList({"cw cwl_enabled 1"})
                 && cwPhoneEdges == 2,
                 "CWL selection is adopted locally and still commands Flex");
    commands.clear();

    tx.startTwoToneTune();
    ok &= expect(commands == QStringList({
                     "transmit set tune_mode=two_tone",
                     "transmit tune 1",
                 })
                     && tx.activePttSource() == TransmitModel::PttSource::Tune,
                 "two-tone tune sets mode and tags the tune source before starting");

    tx.applyChanges(td([](TransmitDelta& d){ d.tune = true; }));
    commands.clear();
    tx.toggleTwoToneTune();
    ok &= expect(commands == QStringList({
                     "transmit tune 0",
                     "transmit set tune_mode=single_tone",
                 }),
                 "two-tone tune toggle stops and restores single-tone mode");

    tx.applyChanges(td([](TransmitDelta& d){ d.tune = false; }));
    commands.clear();
    tx.toggleTwoToneTune();
    ok &= expect(commands == QStringList({
                     "transmit set tune_mode=two_tone",
                     "transmit tune 1",
                 }),
                 "two-tone tune toggle starts two-tone when not tuning");

    commands.clear();
    tx.startTune();
    ok &= expect(commands == QStringList({"transmit tune 1"})
                     && tx.activePttSource() == TransmitModel::PttSource::Tune,
                 "single-tone tune tags the tune source before starting");

    commands.clear();
    tx.setTuneMode("single_tone");
    ok &= expect(commands == QStringList({"transmit set tune_mode=single_tone"}),
                 "single-tone tune mode command is accepted");

    commands.clear();
    tx.setTuneMode("invalid");
    ok &= expect(commands.isEmpty(),
                 "invalid tune mode is ignored");

    commands.clear();
    tx.setDexp(true);
    ok &= expect(commands == QStringList({"transmit set compander=1"})
                 && tx.dexpOn()
                 && tx.companderOn(),
                 "DEXP toggle sends compander command and updates local state");

    commands.clear();
    tx.setDexpLevel(42);
    ok &= expect(commands == QStringList({"transmit set compander_level=42"})
                 && tx.dexpLevel() == 42
                 && tx.companderLevel() == 42,
                 "DEXP level sends compander_level command and updates local state");

    tx.applyChanges(td([](TransmitDelta& d){ d.compander = false; d.companderLevel = 17; }));
    ok &= expect(!tx.dexpOn()
                 && !tx.companderOn()
                 && tx.dexpLevel() == 17
                 && tx.companderLevel() == 17,
                 "compander status updates DEXP state");

    tx.applyChanges(td([](TransmitDelta& d){ d.compander = true; d.companderLevel = 33; }));
    ok &= expect(tx.dexpOn()
                 && tx.companderOn()
                 && tx.dexpLevel() == 33
                 && tx.companderLevel() == 33,
                 "canonical compander status wins over legacy DEXP aliases");

    tx.applyChanges(td([](TransmitDelta& d){ d.compander = false; d.companderLevel = 8; }));
    ok &= expect(!tx.dexpOn()
                 && !tx.companderOn()
                 && tx.dexpLevel() == 8
                 && tx.companderLevel() == 8,
                 "legacy DEXP aliases update state when canonical compander status is absent");

    tx.setPttPreflight([](TransmitModel::PttSource source) {
        return source == TransmitModel::PttSource::Tune
            ? QStringLiteral("blocked")
            : QString();
    });

    blockedMessages.clear();
    commands.clear();
    tx.startTune();
    ok &= expect(commands.isEmpty()
                 && blockedMessages == QStringList({"blocked"}),
                 "tune preflight blocks tune command");

    blockedMessages.clear();
    commands.clear();
    tx.startTwoToneTune();
    ok &= expect(commands.isEmpty()
                 && blockedMessages == QStringList({"blocked"}),
                 "two-tone preflight blocks setup and tune commands");

    commands.clear();
    tx.loadProfile(QStringLiteral("Contest"));
    ok &= expect(commands == QStringList({"profile tx load \"Contest\""}),
                 "TX profile load uses profile tx load");

    commands.clear();
    tx.loadMicProfile(QStringLiteral("Studio Mic"));
    ok &= expect(commands == QStringList({"profile mic load \"Studio Mic\""}),
                 "Mic profile load uses profile mic load");

    // #4161 / #4310: rf_power and tune_power need a dedicated signal so TCI
    // can announce them. A coarse stateChanged() listener cannot tell a power
    // move apart from any other TX field, and the radio restores per-band
    // power on QSY — the case that left control-surface dials stale.
    QList<int> rfPowers;
    QList<int> tunePowers;
    QObject::connect(&tx, &TransmitModel::rfPowerChanged,
                     [&rfPowers](int w) { rfPowers.append(w); });
    QObject::connect(&tx, &TransmitModel::tunePowerChanged,
                     [&tunePowers](int w) { tunePowers.append(w); });

    // Radio-originated: the band-switch path. This is the #4310 regression.
    tx.applyChanges(td([](TransmitDelta& d) { d.rfPower = 30; }));
    ok &= expect(rfPowers == QList<int>({30}),
                 "radio-reported rf_power emits rfPowerChanged");

    tx.applyChanges(td([](TransmitDelta& d) { d.tunePower = 15; }));
    ok &= expect(tunePowers == QList<int>({15}),
                 "radio-reported tune_power emits tunePowerChanged");

    // An unchanged value must not re-announce, or every status refresh would
    // re-broadcast to every connected TCI client.
    tx.applyChanges(td([](TransmitDelta& d) { d.rfPower = 30; }));
    ok &= expect(rfPowers == QList<int>({30}),
                 "unchanged rf_power does not re-emit");

    // Locally-originated: GUI slider or a TCI client's SET.
    rfPowers.clear();
    tunePowers.clear();
    tx.setRfPower(75);
    ok &= expect(rfPowers == QList<int>({75}),
                 "setRfPower emits rfPowerChanged");

    tx.setTunePower(20);
    ok &= expect(tunePowers == QList<int>({20}),
                 "setTunePower emits tunePowerChanged");

    rfPowers.clear();
    tx.setRfPower(75);
    ok &= expect(rfPowers.isEmpty(),
                 "setRfPower to the same value does not re-emit");
    commands.clear();
    tx.setTxFilter(1200, 1800);
    ok &= expect(commands == QStringList({
                     "transmit set filter_low=1200 filter_high=1800",
                 }),
                 "paired TX filter update is sent atomically");

    // ── TX passband: optimistic adoption + the operator-intent signal ───────
    //
    // Both halves matter on a radio that modulates on this host. There is no
    // status echo there, so a setter that only emitted the Flex verb would leave
    // the model — and therefore the Phone applet's sliders — showing the old
    // passband while the modulator kept its mode default. And the backend needs
    // an intent signal it can bind to that applyStatus() never emits, or radio
    // state would be echoed back as a fresh command.
    {
        QList<QPair<int, int>> intents;
        QObject::connect(&tx, &TransmitModel::txFilterCommandIssued,
                         [&intents](int lo, int hi) { intents.append({lo, hi}); });
        commands.clear();
        tx.setTxFilter(100, 4000);
        ok &= expect(tx.txFilterLow() == 100 && tx.txFilterHigh() == 4000,
                     "setTxFilter adopts the passband without waiting for a status echo");
        ok &= expect(intents == QList<QPair<int, int>>({{100, 4000}}),
                     "setTxFilter announces operator intent for the seam");

        // The single-edge setters have to carry the other edge through, or
        // dragging one slider resets the other to whatever it was constructed
        // with.
        intents.clear();
        tx.setTxFilterLow(200);
        ok &= expect(tx.txFilterLow() == 200 && tx.txFilterHigh() == 4000,
                     "setTxFilterLow preserves the high cut");
        tx.setTxFilterHigh(3500);
        ok &= expect(tx.txFilterLow() == 200 && tx.txFilterHigh() == 3500,
                     "setTxFilterHigh preserves the low cut");

        // eSSB has to survive the bounds. 100..6000 is a real setting, not an
        // edge case, and a clamp at 3 kHz would silently make the control a lie.
        tx.setTxFilter(100, 6000);
        ok &= expect(tx.txFilterLow() == 100 && tx.txFilterHigh() == 6000,
                     "an eSSB passband is not clamped away");

        // Radio status must NOT look like operator intent — otherwise a Flex's
        // own echo would be sent straight back out as a command.
        intents.clear();
        TransmitDelta echo;
        echo.txFilterLow = 300;
        echo.txFilterHigh = 2700;
        tx.applyChanges(echo);
        ok &= expect(tx.txFilterLow() == 300 && tx.txFilterHigh() == 2700,
                     "radio status still updates the passband");
        ok &= expect(intents.isEmpty(),
                     "applying radio status does not emit operator intent");
    }

    // ── Mic level: the same operator-intent contract ────────────────────────
    //
    // Hl2TxDsp::setMicGain had no production caller: the slider's `transmit set
    // miclevel=` is dropped by a backend with no command plane, so mic gain did
    // nothing at all on the HL2 and an operator sweeping it end to end saw no
    // change on the air. micLevelCommandIssued is the seam's route in.
    //
    // The applyChanges half is the one that bites if it is wrong. A Flex echoes
    // miclevel back in transmit status, and if that echo looked like intent it
    // would be handed straight back to the seam as a fresh command.
    {
        QList<int> micIntents;
        QObject::connect(&tx, &TransmitModel::micLevelCommandIssued,
                         [&micIntents](int level) { micIntents.append(level); });

        micIntents.clear();
        tx.setMicLevel(80);
        ok &= expect(tx.micLevel() == 80, "setMicLevel adopts the level");
        ok &= expect(micIntents == QList<int>({80}),
                     "setMicLevel announces operator intent for the seam");

        // Re-asserting the SAME level must still reach the seam. A
        // host-modulating backend can have been reset underneath the model — a
        // reconnect, a radio swap — while m_micLevel never moved, and gating on
        // "changed" would leave the modulator at its default with the slider
        // insisting otherwise.
        micIntents.clear();
        tx.setMicLevel(80);
        ok &= expect(micIntents == QList<int>({80}),
                     "re-setting an unchanged mic level still re-asserts to the seam");

        // Out of range is clamped before it is announced, so the seam never has
        // to defend itself against a CAT client's arithmetic.
        micIntents.clear();
        tx.setMicLevel(150);
        ok &= expect(tx.micLevel() == 100 && micIntents == QList<int>({100}),
                     "an over-range mic level is clamped before it reaches the seam");

        micIntents.clear();
        TransmitDelta micEcho;
        micEcho.micLevel = 42;
        tx.applyChanges(micEcho);
        ok &= expect(tx.micLevel() == 42, "radio status still updates the mic level");
        ok &= expect(micIntents.isEmpty(),
                     "applying radio status does not emit mic operator intent");
    }

    ClientQuindarTone quindar;
    quindar.prepare(24000.0);
    quindar.setEnabled(true);
    tx.setQuindarTone(&quindar);
    tx.setTxModeGetter([] { return QStringLiteral("USB"); });
    commands.clear();
    tx.requestPttOn(TransmitModel::PttSource::Wspr);
    ok &= expect(commands == QStringList({"xmit 1"})
                 && quindar.phase() == ClientQuindarTone::Phase::Idle,
                 "WSPR PTT bypasses Quindar signaling");

    // ── ATU tune failure and user abort tests ──
    {
        TransmitModel atuTx;
        QList<QPair<ATUStatus, QString>> failures;
        QObject::connect(&atuTx, &TransmitModel::atuTuneFailed,
                         [&failures](ATUStatus s, const QString& msg) {
                             failures.append(qMakePair(s, msg));
                         });

        // 1. InProgress -> FailBypass emits atuTuneFailed
        atuTx.applyChanges(td([](TransmitDelta& d) { d.atuStatusRaw = QStringLiteral("TUNE_IN_PROGRESS"); }));
        failures.clear();
        atuTx.applyChanges(td([](TransmitDelta& d) { d.atuStatusRaw = QStringLiteral("TUNE_FAIL_BYPASS"); }));
        ok &= expect(failures.size() == 1
                     && failures.first().first == ATUStatus::FailBypass
                     && !failures.first().second.isEmpty(),
                     "InProgress -> FailBypass emits atuTuneFailed");

        // 2. InProgress -> Fail emits atuTuneFailed
        atuTx.applyChanges(td([](TransmitDelta& d) { d.atuStatusRaw = QStringLiteral("TUNE_IN_PROGRESS"); }));
        failures.clear();
        atuTx.applyChanges(td([](TransmitDelta& d) { d.atuStatusRaw = QStringLiteral("TUNE_FAIL"); }));
        ok &= expect(failures.size() == 1
                     && failures.first().first == ATUStatus::Fail
                     && !failures.first().second.isEmpty(),
                     "InProgress -> Fail emits atuTuneFailed");

        // 3. InProgress -> Successful does NOT emit atuTuneFailed
        atuTx.applyChanges(td([](TransmitDelta& d) { d.atuStatusRaw = QStringLiteral("TUNE_IN_PROGRESS"); }));
        failures.clear();
        atuTx.applyChanges(td([](TransmitDelta& d) { d.atuStatusRaw = QStringLiteral("TUNE_SUCCESSFUL"); }));
        ok &= expect(failures.isEmpty(),
                     "InProgress -> Successful does not emit atuTuneFailed");

        // 4. InProgress -> OK does NOT emit atuTuneFailed
        atuTx.applyChanges(td([](TransmitDelta& d) { d.atuStatusRaw = QStringLiteral("TUNE_IN_PROGRESS"); }));
        failures.clear();
        atuTx.applyChanges(td([](TransmitDelta& d) { d.atuStatusRaw = QStringLiteral("TUNE_OK"); }));
        ok &= expect(failures.isEmpty(),
                     "InProgress -> OK does not emit atuTuneFailed");

        // 5. User aborts via atuBypass() while in progress -> FailBypass/Aborted does NOT emit atuTuneFailed
        atuTx.applyChanges(td([](TransmitDelta& d) { d.atuStatusRaw = QStringLiteral("TUNE_IN_PROGRESS"); }));
        atuTx.atuBypass();
        failures.clear();
        atuTx.applyChanges(td([](TransmitDelta& d) { d.atuStatusRaw = QStringLiteral("TUNE_FAIL_BYPASS"); }));
        ok &= expect(failures.isEmpty(),
                     "User-aborted tune does not emit atuTuneFailed on FailBypass");

        // 6. Direct FailBypass on connect without prior InProgress does NOT emit atuTuneFailed
        TransmitModel freshTx;
        failures.clear();
        QObject::connect(&freshTx, &TransmitModel::atuTuneFailed,
                         [&failures](ATUStatus s, const QString& msg) {
                             failures.append(qMakePair(s, msg));
                         });
        freshTx.applyChanges(td([](TransmitDelta& d) { d.atuStatusRaw = QStringLiteral("TUNE_FAIL_BYPASS"); }));
        ok &= expect(failures.isEmpty(),
                     "Initial status FailBypass without InProgress does not emit atuTuneFailed");
    }

    return ok ? 0 : 1;
}
