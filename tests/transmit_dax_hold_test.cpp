// RadioModel's `transmit dax` holder table (#6538, #6579). The flag is one bit
// for the whole radio; TCI, RADE, the voice keyer, AX.25, WSPR and RX Playback
// all need it on while they transmit. The table must hand back what the
// operator had, in whatever order the holders let go, and only once the radio
// is in RX. Socket-free: a bare RadioModel; the radio's TX state is set
// directly and its RX report emitted by hand.
// Run: ./build/transmit_dax_hold_test

#include "core/backends/ModelControlRequest.h"
#include "core/backends/TransmitDelta.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>

#include <cstdio>
#include <string>
#include <variant>

namespace AetherSDR {
class TransmitDaxHoldTestAccess {
public:
    static void setRadioTransmitting(RadioModel& radio, bool on)
    {
        radio.m_radioTransmitting = on;
        if (!on) {
            emit radio.radioTransmittingChanged(false);
        }
    }
    static void disconnect(RadioModel& radio) { radio.onDisconnected(); }
};
} // namespace AetherSDR

using namespace AetherSDR;
using Holder = RadioModel::DaxTxFlagHolder;
using Access = TransmitDaxHoldTestAccess;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

// Counts the `transmit dax` writes the model asks the backend for, so "no
// command" is asserted rather than inferred from an unchanged value.
struct DaxWrites {
    int count{0};
    explicit DaxWrites(TransmitModel& tx)
    {
        QObject::connect(&tx, &TransmitModel::controlRequested, &tx,
                         [this](const TransmitControlRequest& request) {
            if (request.field == TransmitControlRequest::Field::Dax) ++count;
        });
    }
};

std::string state(const TransmitModel& tx, const DaxWrites& writes)
{
    return "dax=" + std::to_string(tx.daxOn()) + " writes=" + std::to_string(writes.count);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // One holder, DAX off before: on while held, off after.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(false);
        radio.acquireTransmitDax(Holder::Tci);
        const bool onWhileHeld = tx.daxOn();
        radio.releaseTransmitDax(Holder::Tci);
        report("single_hold_sets_then_restores_off", onWhileHeld && !tx.daxOn());
    }

    // DAX already on (SmartSDR DAX2 had it): nothing to write either way.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(true);
        DaxWrites writes(tx);
        radio.acquireTransmitDax(Holder::Tci);
        radio.releaseTransmitDax(Holder::Tci);
        report("baseline_on_sends_no_command", tx.daxOn() && writes.count == 0,
               state(tx, writes));
    }

    // The operator's scenario: TCI takes it, RADE takes it, TCI lets go
    // first. RADE must keep the flag; only RADE's release hands it back.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(false);
        radio.acquireTransmitDax(Holder::Tci);
        radio.acquireTransmitDax(Holder::Rade);
        radio.releaseTransmitDax(Holder::Tci);
        const bool keptForRade = tx.daxOn();
        radio.releaseTransmitDax(Holder::Rade);
        report("first_out_release_keeps_flag_for_other_holder",
               keptForRade && !tx.daxOn());
    }

    // The same overlap released in the other order.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(false);
        radio.acquireTransmitDax(Holder::Rade);
        radio.acquireTransmitDax(Holder::Tci);
        radio.releaseTransmitDax(Holder::Rade);
        const bool keptForTci = tx.daxOn();
        radio.releaseTransmitDax(Holder::Tci);
        report("last_in_release_keeps_flag_for_other_holder",
               keptForTci && !tx.daxOn());
    }

    // A release while the carrier is up waits for the radio's RX report:
    // switching the modulator to the mic mid-over would put the room on air.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(false);
        radio.acquireTransmitDax(Holder::Tci);
        Access::setRadioTransmitting(radio, true);
        radio.releaseTransmitDax(Holder::Tci);
        const bool heldWhileKeyed = tx.daxOn();
        Access::setRadioTransmitting(radio, false);
        report("release_waits_for_rx", heldWhileKeyed && !tx.daxOn());
    }

    // A key-up that arrives while that restore is still waiting keeps the
    // operator's value, not the dax=1 the first hold put there.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(false);
        radio.acquireTransmitDax(Holder::Tci);
        Access::setRadioTransmitting(radio, true);
        radio.releaseTransmitDax(Holder::Tci);
        radio.acquireTransmitDax(Holder::Tci);
        Access::setRadioTransmitting(radio, false);
        const bool stillHeld = tx.daxOn();
        radio.releaseTransmitDax(Holder::Tci);
        report("reacquire_before_rx_keeps_original_baseline",
               stillHeld && !tx.daxOn());
    }

    // The operator (or another client) turns DAX off mid-hold: theirs is the
    // newer choice, and the release must not write over it.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(false);
        radio.acquireTransmitDax(Holder::Rade);
        tx.setDax(false);
        DaxWrites writes(tx);
        radio.releaseTransmitDax(Holder::Rade);
        report("outside_turn_off_is_left_alone", !tx.daxOn() && writes.count == 0,
               state(tx, writes));
    }

    // Releasing a holder that holds nothing writes nothing and frees nothing.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(false);
        radio.acquireTransmitDax(Holder::Wspr);
        DaxWrites writes(tx);
        radio.releaseTransmitDax(Holder::Ax25);
        report("release_of_non_holder_is_a_no_op",
               tx.daxOn() && writes.count == 0
                   && radio.transmitDaxHeldBy(Holder::Wspr)
                   && !radio.transmitDaxHeldBy(Holder::Ax25),
               state(tx, writes));
    }

    // A dropped session takes the flag with it: a producer that releases
    // after the disconnect finds no hold and sends nothing.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(false);
        radio.acquireTransmitDax(Holder::VoiceKeyer);
        Access::disconnect(radio);
        const bool droppedAtDisconnect = !radio.transmitDaxHeldBy(Holder::VoiceKeyer);
        DaxWrites writes(tx);
        radio.releaseTransmitDax(Holder::VoiceKeyer);
        report("disconnect_drops_every_hold",
               droppedAtDisconnect && writes.count == 0, state(tx, writes));
    }

    // #6593 review (rfoust): the radio's echo of an EARLIER dax=1 lands after
    // the restore's dax=0 was sent but before it was answered. The next
    // holder must still send dax=1, and must not take that stale 1 as the
    // operator's baseline, or its release would leave the flag latched.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(false);
        radio.acquireTransmitDax(Holder::Rade);   // requests ON
        radio.releaseTransmitDax(Holder::Rade);   // requests OFF while RX

        TransmitDelta onEcho;
        onEcho.dax = true;                        // earlier ON status arrives late
        tx.applyChanges(onEcho);
        DaxWrites writes(tx);
        radio.acquireTransmitDax(Holder::Tci);    // must still ask for ON

        TransmitDelta offEcho;
        offEcho.dax = false;                      // outstanding OFF reaches the radio
        tx.applyChanges(offEcho);
        report("delayed_on_echo_must_not_suppress_next_acquire",
               writes.count > 0,
               state(tx, writes) + " holder="
                   + std::to_string(radio.transmitDaxHeldBy(Holder::Tci)));

        TransmitDelta tciOnEcho;
        tciOnEcho.dax = true;                     // TCI's own ON lands
        tx.applyChanges(tciOnEcho);
        radio.releaseTransmitDax(Holder::Tci);
        report("delayed_on_echo_is_not_taken_as_baseline", !tx.daxOn(),
               state(tx, writes));
    }

    // The same race started by the operator rather than a holder: DAX clicked
    // on, then off, and the echo of the ON overtakes the OFF.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(true);
        tx.setDax(false);
        TransmitDelta staleOn;
        staleOn.dax = true;
        tx.applyChanges(staleOn);
        DaxWrites writes(tx);
        radio.acquireTransmitDax(Holder::Tci);
        radio.releaseTransmitDax(Holder::Tci);
        report("operator_stale_echo_neither_suppresses_nor_latches",
               writes.count == 2 && !tx.daxOn(), state(tx, writes));
    }

    // A dax request that never reaches a radio (here: no backend at all, as
    // when disconnected) is still answered, so the flag cannot read as in
    // flight for the rest of the session.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(true);
        const bool pendingAtFirst = !tx.daxSettled();
        QCoreApplication::processEvents();
        report("unsent_dax_request_is_answered", pendingAtFirst && tx.daxSettled());
    }

    std::printf("%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
