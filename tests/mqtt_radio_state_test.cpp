// aethersdr/radio/state payload + timing contract (#5518).
//
// The topic grew `drive` / `max_power_level` for an amplifier interlock, so the
// states this test cares about are the ones where a naive implementation
// publishes something plausible and wrong: before the radio has reported power
// at all, after a disconnect, and with no slice open. All three are ordering
// states, which is why the payload builder is a pure function over an input
// struct rather than a body inside MainWindow — they can be driven directly.
//
// The second half drives the real TransmitModel and a QTimer configured exactly
// as MainWindow_Spots.cpp configures m_radioStateCoalesceTimer, because the
// have-status latch and the debounce are where #5518's findings actually live.

#include "core/MqttRadioState.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>

#include <iostream>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

// A connected Flex mid-session: slice up, power reported.
MqttRadioStateInputs liveInputs()
{
    MqttRadioStateInputs in;
    in.connected           = true;
    in.transmitting        = false;
    in.haveSlice           = true;
    in.sliceLetter         = QStringLiteral("A");
    in.sliceFrequencyMhz   = 14.074;
    in.sliceMode           = QStringLiteral("DIGU");
    in.haveTransmitStatus  = true;
    in.drive               = 10;
    in.haveMaxPowerLevel   = true;
    in.maxPowerLevel       = 100;
    in.driveIsReadback     = true;
    return in;
}

// Pump the event loop until `predicate` holds or `budgetMs` elapses. Returns
// whether the predicate held; a timeout is a failure the caller reports, not a
// hang.
template <class Predicate>
bool pumpUntil(Predicate predicate, int budgetMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!predicate() && clock.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return predicate();
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;

    // ── Payload shape, the ordinary case ────────────────────────────────────
    {
        const QJsonObject obj = buildMqttRadioStatePayload(liveInputs());
        ok &= expect(obj.value(QStringLiteral("slice")).toString() == QStringLiteral("A"),
                     "live payload carries slice letter");
        ok &= expect(qFuzzyCompare(obj.value(QStringLiteral("freq")).toDouble(), 14.074),
                     "live payload carries slice frequency in MHz");
        ok &= expect(obj.value(QStringLiteral("mode")).toString() == QStringLiteral("DIGU"),
                     "live payload carries slice mode");
        ok &= expect(obj.value(QStringLiteral("tx")).toBool() == false,
                     "live payload carries tx");
        ok &= expect(obj.value(QStringLiteral("connected")).toBool() == true,
                     "live payload carries connected");
        ok &= expect(obj.value(QStringLiteral("drive")).toInt() == 10,
                     "live payload carries raw 0..100 drive");
        ok &= expect(obj.value(QStringLiteral("max_power_level")).toInt() == 100,
                     "live payload carries max_power_level beside drive");
        ok &= expect(obj.value(QStringLiteral("drive_confirmed")).toBool() == true,
                     "readback backend reports drive_confirmed true");
    }

    // ── Drive is the RAW SETTING, not watts. A 500 W PGXL running 50% must not
    //    be mistaken for 50 W by a subscriber, which is the entire reason
    //    max_power_level ships alongside it.
    {
        MqttRadioStateInputs in = liveInputs();
        in.drive         = 50;
        in.maxPowerLevel = 500;
        const QJsonObject obj = buildMqttRadioStatePayload(in);
        ok &= expect(obj.value(QStringLiteral("drive")).toInt() == 50,
                     "drive stays the raw percent on a 500 W rig");
        ok &= expect(obj.value(QStringLiteral("max_power_level")).toInt() == 500,
                     "max_power_level carries the real ceiling, not 100");
    }

    // ── No active slice: the power fields must still publish. Amplifier logic
    //    cannot depend on a slice existing (the Mission requirement on #5518).
    {
        MqttRadioStateInputs in = liveInputs();
        in.haveSlice = false;
        in.sliceLetter.clear();
        in.sliceFrequencyMhz = 0.0;
        in.sliceMode.clear();
        const QJsonObject obj = buildMqttRadioStatePayload(in);
        ok &= expect(!obj.contains(QStringLiteral("slice"))
                         && !obj.contains(QStringLiteral("freq"))
                         && !obj.contains(QStringLiteral("mode")),
                     "slice-less payload omits the slice fields entirely");
        ok &= expect(obj.value(QStringLiteral("drive")).toInt() == 10,
                     "slice-less payload still carries drive");
        ok &= expect(obj.contains(QStringLiteral("max_power_level")),
                     "slice-less payload still carries max_power_level");
        ok &= expect(obj.contains(QStringLiteral("tx"))
                         && obj.contains(QStringLiteral("connected")),
                     "slice-less payload still carries radio-level tx/connected");
    }

    // ── Before the first transmit status: OMITTED, not published as the class
    //    default. A phantom 100 is indistinguishable from confirmed full drive.
    {
        MqttRadioStateInputs in = liveInputs();
        in.haveTransmitStatus = false;
        in.drive              = 100;   // the TransmitModel class default
        in.haveMaxPowerLevel  = false;
        in.maxPowerLevel      = 100;
        const QJsonObject obj = buildMqttRadioStatePayload(in);
        ok &= expect(!obj.contains(QStringLiteral("drive")),
                     "unreported drive is absent, never a default 100");
        ok &= expect(!obj.contains(QStringLiteral("max_power_level")),
                     "unreported max_power_level is absent too");
        ok &= expect(!obj.contains(QStringLiteral("drive_confirmed")),
                     "drive_confirmed is absent when there is no drive to qualify");
        ok &= expect(obj.contains(QStringLiteral("slice"))
                         && obj.contains(QStringLiteral("tx")),
                     "missing power does not suppress the rest of the payload");
    }

    // ── Disconnect: connected:false and no power fields, so the topic does not
    //    carry a dead session's drive into the next one.
    {
        MqttRadioStateInputs in;   // defaults ARE the disconnected reading
        const QJsonObject obj = buildMqttRadioStatePayload(in);
        ok &= expect(obj.value(QStringLiteral("connected")).toBool() == false,
                     "disconnected payload says connected:false");
        ok &= expect(obj.value(QStringLiteral("tx")).toBool() == false,
                     "disconnected payload says tx:false");
        ok &= expect(!obj.contains(QStringLiteral("drive"))
                         && !obj.contains(QStringLiteral("max_power_level")),
                     "disconnected payload retires the power fields");
    }

    // ── Disconnect with the latches STILL SET, which is the real wire order.
    //    RadioModel::onDisconnected() emits radioTransmittingChanged(false) ten
    //    lines before it calls TransmitModel::resetState(), and that signal is
    //    wired to a direct, uncoalesced publish -- so the payload is built while
    //    haveTransmitStatus/driveIsReadback are still true. The case above uses
    //    default inputs and therefore never exercised this (#5733 review).
    {
        MqttRadioStateInputs in = liveInputs();
        in.connected          = false;   // isConnected() already reads false here
        in.haveTransmitStatus = true;    // resetState() has NOT run yet
        in.drive              = 100;
        in.driveIsReadback    = true;
        in.haveMaxPowerLevel  = true;
        in.maxPowerLevel      = 500;
        const QJsonObject obj = buildMqttRadioStatePayload(in);
        ok &= expect(!obj.contains(QStringLiteral("drive")),
                     "a dead radio's drive is retired even with the latch still set");
        ok &= expect(!obj.contains(QStringLiteral("max_power_level")),
                     "a dead radio's ceiling is retired even with the latch still set");
        ok &= expect(!obj.contains(QStringLiteral("drive_confirmed")),
                     "no drive_confirmed:true on a connected:false payload");
        ok &= expect(obj.value(QStringLiteral("connected")).toBool() == false,
                     "the disconnect payload still says connected:false");
    }

    // ── An intent-only backend (HL2) must say so: drive present, but flagged.
    {
        MqttRadioStateInputs in = liveInputs();
        in.driveIsReadback = false;
        in.drive           = 100;
        const QJsonObject obj = buildMqttRadioStatePayload(in);
        ok &= expect(obj.value(QStringLiteral("drive_confirmed")).toBool() == false,
                     "intent-only backend reports drive_confirmed false");
        ok &= expect(obj.value(QStringLiteral("drive")).toInt() == 100,
                     "intent-only backend still publishes the requested drive");
    }

    // ── tx is never inferred from drive. Drive 0 while keyed is a real state
    //    (HL2 gated, or an operator at zero power) and must not read as tx:false.
    {
        MqttRadioStateInputs in = liveInputs();
        in.transmitting = true;
        in.drive        = 0;
        const QJsonObject obj = buildMqttRadioStatePayload(in);
        ok &= expect(obj.value(QStringLiteral("tx")).toBool() == true,
                     "tx stays true with drive 0 — never inferred from power");
        ok &= expect(obj.contains(QStringLiteral("drive"))
                         && obj.value(QStringLiteral("drive")).toInt() == 0,
                     "a reported drive of 0 publishes as 0, not as absent");
    }

    // ── The ceiling gates INDEPENDENTLY of drive (#5733). Only FlexBackend
    //    populates TransmitDelta::maxPowerLevel; on an Icom the drive latch says
    //    nothing about the ceiling, and publishing the class default as a
    //    firmware answer turns a 10 W IC-705 into a 50 W one for a subscriber
    //    computing watts.
    {
        MqttRadioStateInputs in = liveInputs();
        in.drive              = 50;
        in.haveMaxPowerLevel  = false;
        in.maxPowerLevel      = 100;   // the class default, never reported
        const QJsonObject obj = buildMqttRadioStatePayload(in);
        ok &= expect(obj.value(QStringLiteral("drive")).toInt() == 50,
                     "reported drive publishes without a reported ceiling");
        ok &= expect(!obj.contains(QStringLiteral("max_power_level")),
                     "an unreported ceiling is absent, never the class default");
        ok &= expect(obj.contains(QStringLiteral("drive_confirmed")),
                     "drive_confirmed still qualifies the drive that IS reported");
    }

    // ── drive_confirmed describes THIS VALUE, not the backend (#5733). A Flex
    //    reads drive back, but an operator-set value is a REQUEST until the
    //    radio echoes it — publishing it as confirmed is the Principle II
    //    violation the flag exists to prevent.
    {
        MqttRadioStateInputs in = liveInputs();
        in.driveIsReadback = false;   // readback backend, but this value is ours
        in.drive           = 75;
        const QJsonObject obj = buildMqttRadioStatePayload(in);
        ok &= expect(obj.value(QStringLiteral("drive_confirmed")).toBool() == false,
                     "an unacknowledged local set is not confirmed on a readback radio");
        ok &= expect(obj.value(QStringLiteral("drive")).toInt() == 75,
                     "the requested drive still publishes while unconfirmed");
    }

    // ── The have-status latch on the real model ─────────────────────────────
    {
        TransmitModel tm;
        ok &= expect(!tm.haveTransmitStatus(),
                     "fresh model has not seen a transmit status");
        ok &= expect(tm.rfPower() == 100,
                     "fresh model holds the 100 default that must stay unpublished");

        // The value-identical case: a radio reporting 100 into a model already at
        // 100 changes nothing, so a latch keyed on the change would never fire for
        // the exact value it most needs to confirm.
        ok &= expect(!tm.rfPowerIsFromRadio(),
                     "fresh model's drive is not radio-confirmed");
        ok &= expect(!tm.haveMaxPowerLevel(),
                     "fresh model has not seen a ceiling either");

        // The value-identical case: a radio reporting 100 into a model already at
        // 100 changes nothing, so a latch keyed on the change would never fire for
        // the exact value it most needs to confirm — and neither would any
        // value-change SIGNAL, which is why powerProvenanceChanged exists.
        int provenance = 0;
        QObject::connect(&tm, &TransmitModel::powerProvenanceChanged,
                         &tm, [&provenance] { ++provenance; });
        TransmitDelta same;
        same.rfPower = 100;
        tm.applyChanges(same);
        ok &= expect(tm.haveTransmitStatus(),
                     "status equal to the default still latches have-status");
        ok &= expect(provenance == 1,
                     "the latch flip announces itself when no value moved");
        ok &= expect(tm.rfPowerIsFromRadio(),
                     "a reported drive is radio-confirmed");

        // An operator set demotes it to a request, again with no value change.
        tm.setRfPower(100);
        ok &= expect(!tm.rfPowerIsFromRadio(),
                     "a local set makes drive unconfirmed until the radio echoes");
        ok &= expect(provenance == 2,
                     "the demotion announces itself when no value moved");

        TransmitDelta moved;
        moved.rfPower = 27;
        tm.applyChanges(moved);
        ok &= expect(tm.rfPower() == 27 && tm.haveTransmitStatus(),
                     "reported drive applies and stays latched");

        tm.resetState();
        ok &= expect(!tm.haveTransmitStatus(),
                     "disconnect clears have-status so 100 is a default again");
        ok &= expect(!tm.rfPowerIsFromRadio() && !tm.haveMaxPowerLevel(),
                     "disconnect clears the provenance and ceiling latches too");
        ok &= expect(tm.rfPower() == 100,
                     "disconnect restores the drive default");

        // The narrow teardown-path reset clears exactly the same three latches.
        // resetState() delegates to it, so this pins them together: a family
        // switch and a disconnect must agree on what "nobody has reported" means.
        TransmitDelta again2;
        again2.rfPower = 55;
        again2.maxPowerLevel = 500;
        tm.applyChanges(again2);
        ok &= expect(tm.haveTransmitStatus() && tm.haveMaxPowerLevel()
                         && tm.rfPowerIsFromRadio(),
                     "a fresh report re-latches all three");
        tm.resetPowerProvenance();
        ok &= expect(!tm.haveTransmitStatus() && !tm.haveMaxPowerLevel()
                         && !tm.rfPowerIsFromRadio(),
                     "resetPowerProvenance clears exactly what resetState clears");
        ok &= expect(tm.rfPower() == 55 && tm.maxPowerLevel() == 500,
                     "and leaves the values alone - it emits nothing, so it must");

        // resetState must NOT emit rfPowerChanged: that signal drives a TCI
        // `drive:` broadcast and the TX meter scale, and a departing radio did not
        // move its power to 100.
        int emits = 0;
        QObject::connect(&tm, &TransmitModel::rfPowerChanged,
                         &tm, [&emits](int) { ++emits; });
        TransmitDelta again;
        again.rfPower = 42;
        tm.applyChanges(again);
        ok &= expect(emits == 1, "a reported change emits rfPowerChanged once");
        tm.resetState();
        ok &= expect(emits == 1, "resetState does not emit a phantom rfPowerChanged");
    }

    // ── Coalescing: a slider drag must produce ONE publish carrying the FINAL
    //    value, not a publish per step and not an intermediate value.
    {
        TransmitModel tm;
        QTimer coalesce;                 // same shape as m_radioStateCoalesceTimer
        coalesce.setSingleShot(true);
        coalesce.setInterval(150);

        int publishes = 0;
        int lastDrive = -1;
        QObject::connect(&coalesce, &QTimer::timeout, &tm, [&] {
            ++publishes;
            MqttRadioStateInputs in;
            in.connected          = true;
            in.haveTransmitStatus = tm.haveTransmitStatus();
            in.drive              = tm.rfPower();
            in.haveMaxPowerLevel  = tm.haveMaxPowerLevel();
            in.maxPowerLevel      = tm.maxPowerLevel();
            const QJsonObject obj = buildMqttRadioStatePayload(in);
            lastDrive = obj.contains(QStringLiteral("drive"))
                            ? obj.value(QStringLiteral("drive")).toInt()
                            : -1;
        });
        // BOTH production edges, because either alone is a different test:
        // rfPowerChanged cannot fire for a value-identical report, and that case
        // is the one that left an operator running full drive with no `drive` on
        // the topic at all (#5733).
        QObject::connect(&tm, &TransmitModel::rfPowerChanged,
                         &coalesce, [&](int) { coalesce.start(); });
        QObject::connect(&tm, &TransmitModel::powerProvenanceChanged,
                         &coalesce, [&] { coalesce.start(); });

        for (int step : {20, 30, 40, 50, 60}) {   // the drag
            TransmitDelta d;
            d.rfPower = step;
            tm.applyChanges(d);
        }
        ok &= expect(publishes == 0, "no publish lands mid-drag");
        ok &= expect(pumpUntil([&] { return publishes > 0; }, 2000),
                     "the coalesced publish arrives after the drag settles");
        ok &= expect(publishes == 1, "five drive steps coalesce into one publish");
        ok &= expect(lastDrive == 60,
                     "the coalesced publish carries the final drive, not an "
                     "intermediate step");

        // The step the original drag could never take: a report EQUAL to what the
        // model already holds. assign() returns false and rfPowerChanged stays
        // silent, so only the provenance edge can carry this — and a mirror that
        // misses it never learns the radio confirmed the value.
        tm.setRfPower(60);                       // demote to a local request
        ok &= expect(pumpUntil([&] { return publishes > 1; }, 2000),
                     "a local set at the same value still reaches the mirror");
        const int afterSet = publishes;

        TransmitDelta echo;
        echo.rfPower = 60;                       // the radio echoes it back
        tm.applyChanges(echo);
        ok &= expect(pumpUntil([&] { return publishes > afterSet; }, 2000),
                     "a value-identical radio report still reaches the mirror");
        ok &= expect(tm.rfPowerIsFromRadio(),
                     "and the echo re-confirms the drive it did not change");
    }

    // ── The provenance signal is THE provenance edge, in both directions and
    //    at the moment it fires (#5733 review).
    {
        TransmitModel tm;

        // F3: a synchronous consumer must see the ceiling the latch announces.
        // m_haveMaxPowerLevel latches at the top of applyChanges() but
        // m_maxPowerLevel is assigned much later, so emitting in between handed
        // a slot haveMaxPowerLevel()==true with the compiled-in default still in
        // place -- the phantom the latch exists to prevent.
        int ceilingSeenBySlot = -1;
        bool latchSeenBySlot = false;
        QObject::connect(&tm, &TransmitModel::powerProvenanceChanged, &tm, [&] {
            latchSeenBySlot   = tm.haveMaxPowerLevel();
            ceilingSeenBySlot = tm.maxPowerLevel();
        });
        TransmitDelta first;
        first.maxPowerLevel = 500;            // first report, unlike the 100 default
        tm.applyChanges(first);
        ok &= expect(latchSeenBySlot,
                     "the provenance slot sees the max-power latch set");
        ok &= expect(ceilingSeenBySlot == 500,
                     "the provenance slot sees the REPORTED ceiling, not the default");
    }
    {
        // F4: a demotion is announced whether or not the value moved. The
        // value-identical case is covered above; this is the one an operator
        // actually performs -- dragging the slider to a different number on a
        // backend that had confirmed the old one.
        TransmitModel tm;
        TransmitDelta confirm;
        confirm.rfPower = 60;
        tm.applyChanges(confirm);             // radio confirms 60
        ok &= expect(tm.rfPowerIsFromRadio(), "the radio report confirms the drive");

        int provenanceEdges = 0;
        QObject::connect(&tm, &TransmitModel::powerProvenanceChanged,
                         &tm, [&] { ++provenanceEdges; });
        tm.setRfPower(40);                    // operator drags 60 -> 40
        ok &= expect(!tm.rfPowerIsFromRadio(),
                     "a local set demotes the drive to a request");
        ok &= expect(provenanceEdges == 1,
                     "the demotion is announced even though the value moved too");
    }

    return ok ? 0 : 1;
}
