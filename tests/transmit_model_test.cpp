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

    return ok ? 0 : 1;
}
