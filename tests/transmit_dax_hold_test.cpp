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

    // #6593 review (jensenpat): "not TRANSMITTING" is not "receiving". A Flex
    // reports UNKEY_REQUESTED on its way out of TX and PTT_REQUESTED on its
    // way in; the restore must wait for READY in both, through the production
    // interlock status router rather than the TX boolean.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(false);
        radio.acquireTransmitDax(Holder::Tci);
        radio.handleStatusForTest("interlock", {{"state", "TRANSMITTING"}});
        radio.releaseTransmitDax(Holder::Tci);
        DaxWrites writes(tx);
        radio.handleStatusForTest("interlock", {{"state", "UNKEY_REQUESTED"}});
        report("unkey_requested_must_not_restore_before_ready",
               tx.daxOn() && writes.count == 0, state(tx, writes));
        radio.handleStatusForTest("interlock", {{"state", "READY"}});
        report("ready_finishes_pending_restore", !tx.daxOn(), state(tx, writes));
    }
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(false);
        radio.acquireTransmitDax(Holder::Tci);
        radio.handleStatusForTest("interlock", {{"state", "PTT_REQUESTED"}});
        DaxWrites writes(tx);
        radio.releaseTransmitDax(Holder::Tci);
        const bool heldThroughRequest = tx.daxOn() && writes.count == 0;
        radio.handleStatusForTest("interlock", {{"state", "READY"}});
        report("release_during_ptt_requested_waits_for_ready",
               heldThroughRequest && !tx.daxOn(), state(tx, writes));
    }
    // A fault is not a settled receive either: wait for it to clear.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(false);
        radio.acquireTransmitDax(Holder::Tci);
        radio.handleStatusForTest("interlock", {{"state", "TRANSMITTING"}});
        radio.releaseTransmitDax(Holder::Tci);
        DaxWrites writes(tx);
        radio.handleStatusForTest("interlock", {{"state", "TX_FAULT"}});
        const bool heldThroughFault = tx.daxOn() && writes.count == 0;
        radio.handleStatusForTest("interlock", {{"state", "READY"}});
        report("tx_fault_waits_for_ready", heldThroughFault && !tx.daxOn(),
               state(tx, writes));
    }

    // Backend replacement can retire a session without onDisconnected().
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(false);
        radio.acquireTransmitDax(Holder::Rade);
        radio.rebuildBackendForTest("sim");
        report("backend_replacement_drops_old_holders",
               !radio.transmitDaxHeldBy(Holder::Rade));
    }
    // The replacement's ON report must become its own borrowing baseline.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(false);
        radio.acquireTransmitDax(Holder::Rade);
        radio.handleStatusForTest("interlock", {{"state", "TRANSMITTING"}});
        radio.releaseTransmitDax(Holder::Rade);
        radio.rebuildBackendForTest("sim");
        radio.rebuildBackendForTest("flex");
        QCoreApplication::processEvents();
        TransmitDelta newRadioOn;
        newRadioOn.dax = true;
        tx.applyChanges(newRadioOn);
        DaxWrites writes(tx);
        radio.acquireTransmitDax(Holder::Tci);
        radio.releaseTransmitDax(Holder::Tci);
        radio.handleStatusForTest("interlock", {{"state", "READY"}});
        report("new_backend_on_baseline_survives_old_pending_restore",
               tx.daxOn() && writes.count == 0, state(tx, writes));
    }

    // A new session's first request cannot be settled by a retired ticket.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        quint64 ticket = 0;
        QObject::connect(&tx, &TransmitModel::controlRequested, &tx,
                         [&ticket](const TransmitControlRequest& request) {
            if (request.field == TransmitControlRequest::Field::Dax) {
                ticket = request.answerTicket;
            }
        });
        tx.setDax(false);
        const quint64 oldTicket = ticket;
        radio.rebuildBackendForTest("sim");
        radio.rebuildBackendForTest("flex");
        const bool retired = tx.daxSettled();
        TransmitDelta newRadioOn;
        newRadioOn.dax = true;
        tx.applyChanges(newRadioOn);
        DaxWrites writes(tx);
        radio.acquireTransmitDax(Holder::Tci);
        radio.releaseTransmitDax(Holder::Tci);
        report("retired_off_request_cannot_override_new_on_report",
               retired && tx.daxOn() && writes.count == 0, state(tx, writes));
        tx.setDax(false);
        const quint64 newTicket = ticket;
        tx.handleControlAnswer(oldTicket, false);
        const bool newRequestPending = !tx.daxSettled();
        tx.handleControlAnswer(newTicket, true);
        report("retired_answer_cannot_consume_new_session_request",
               newTicket > oldTicket && newRequestPending && tx.daxSettled());
    }

    // Neither a transmit report nor an optimistic DAX value is evidence about
    // the replacement. With no new interlock report, the new borrow can finish.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        tx.setDax(true);
        radio.handleStatusForTest("interlock", {{"state", "TRANSMITTING"}});
        radio.rebuildBackendForTest("sim");
        radio.rebuildBackendForTest("flex");
        report("backend_replacement_retires_observed_dax", !tx.daxOn());
        TransmitDelta newRadioOff;
        newRadioOff.dax = false;
        tx.applyChanges(newRadioOff);
        DaxWrites writes(tx);
        radio.acquireTransmitDax(Holder::Tci);
        radio.releaseTransmitDax(Holder::Tci);
        report("backend_replacement_retires_receive_qualification",
               !tx.daxOn() && writes.count == 2, state(tx, writes));
    }

    // Retirement itself never hands an old borrowing back to a replacement.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        radio.acquireTransmitDax(Holder::Rade);
        radio.handleStatusForTest("interlock", {{"state", "TRANSMITTING"}});
        radio.releaseTransmitDax(Holder::Rade);
        DaxWrites writes(tx);
        radio.rebuildBackendForTest("sim");
        radio.handleStatusForTest("interlock", {{"state", "READY"}});
        radio.releaseTransmitDax(Holder::Rade);
        report("backend_retirement_never_restores_onto_replacement", writes.count == 0,
               state(tx, writes));
    }

    // A disconnect's false TX notification must not flush a pending restore.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        radio.acquireTransmitDax(Holder::Tci);
        radio.handleStatusForTest("interlock", {{"state", "TRANSMITTING"}});
        radio.releaseTransmitDax(Holder::Tci);
        DaxWrites writes(tx);
        Access::disconnect(radio);
        report("disconnect_discards_pending_restore_without_request", writes.count == 0,
               state(tx, writes));
    }

    // Unsent edits from retirement callbacks must not supply the next baseline.
    {
        RadioModel radio;
        TransmitModel& tx = radio.transmitModel();
        const QMetaObject::Connection attempt = QObject::connect(
            &radio, &RadioModel::transmitSessionInvalidated, &radio,
            [&tx] { tx.setDax(false); });
        radio.rebuildBackendForTest("sim");
        radio.rebuildBackendForTest("flex");
        QObject::disconnect(attempt);
        const bool settled = tx.daxSettled();
        TransmitDelta newRadioOn;
        newRadioOn.dax = true;
        tx.applyChanges(newRadioOn);
        DaxWrites writes(tx);
        radio.acquireTransmitDax(Holder::Tci);
        radio.releaseTransmitDax(Holder::Tci);
        report("retirement_callback_request_cannot_supply_new_baseline",
               settled && tx.daxOn() && writes.count == 0, state(tx, writes));
    }

    std::printf("%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
