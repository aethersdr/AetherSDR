#pragma once

// ONE predicate for band/segment zoom, because there were two and they
// disagreed.
//
// `band_zoom=`/`segment_zoom=` are FlexLib wire text (SmartSDR pcap; see
// MainWindow::togglePanZoomModeForPan). Only a radio that DECLARES the control
// answers them: RadioModel::sendCmd drops the write at hasCommandPlane() and
// emits commandDropped(), so on a Hermes-Lite 2 the control moves and nothing
// reaches the radio -- the HERMES.md section 17 dead-control shape.
//
// SpectrumWidget::setBandSegmentZoomAvailable() already closed that for the two
// on-screen buttons: MainWindow disables "B" and "S" and gives them a tooltip
// saying why. What it could not close is every OTHER way the same command is
// reached, because those paths do not go through the buttons at all. Counted
// at the surface an operator can actually press, there are SEVEN and the
// buttons were the only gated one:
//
//   1. the two waterfall-corner QPushButtons          (gated, via the above)
//   2. the `band_zoom`/`segment_zoom` shortcut actions (MainWindow::
//      registerShortcutActions -- note both ship with an empty QKeySequence)
//   3. the MIDI bridge's global.bandZoom/global.segmentZoom
//      (MainWindow::registerMidiParams, via its fireShortcut lambda)
//   4. the FlexControl button table  (MainWindow::handleFlexControlButton)
//   5. the RC28 / Stream Deck / T-Mate2 button chain
//      (MainWindow::dispatchHidAction -- a SEPARATE if/else from 4, and live
//      by default: tmate2KeyDefaultAction returns "BandZoom" for key 3)
//   6. the FlexControl / HID / Ulanzi wheel and dial
//      (MainWindow::applyFlexControlWheelAction)
//   7. the automation bridge's `shortcut` verb (AutomationServer::doShortcut)
//
// Six of the seven fan into just two methods -- MainWindow::togglePanZoomMode
// (2, 3, 4, 5, 7, via togglePanZoomModeForPan) and MainWindow::setPanZoomMode
// (6, the explicit-state form) -- and both checked isConnected() and a pan id
// and nothing else, so a disabled button and a live keystroke did the same
// thing on a radio that answers neither. Gating the two methods therefore
// closes all six without needing an edit per surface.
//
// A capability gate that one entry point honours and another bypasses is the
// class #5851 names for a TX gate scoped to a button. This one is not that:
// see "RECEIVE-ONLY BY CONSTRUCTION" below.
//
// THE MIDDLE RUNG IS A DECLARED CAPABILITY, NOT A FAMILY STRING. It is
// `RadioCapabilities::panZoomModes.has_value()` -- a per-feature record a
// backend engages (FlexBackend::capabilities() today, nullopt everywhere else),
// read off RadioModel::backendCapabilities() the way hasDdcPanEdgeRolloff is
// one line away in MainWindow::onConnectionStateChanged. An earlier revision of
// this header asked RadioModel::usesFlexCommandPlane() instead, which is
// literally `family() == "flex"`, and #5554's standing notice in AGENTS.md is
// that no new one of those may be added above the seam. The record route costs
// nothing it was avoided for: the capability RATCHET
// (tools/check_capability_records.py) counts DIRECT bool MEMBERS of
// RadioCapabilities, and an std::optional<PanZoomModes> is not one -- the
// checker's own error text names the record as the sanctioned shape. The
// second family that gains a zoom verb engages the record and needs no edit
// here at all.
//
// A REFUSAL MUST SAY SO. A capability gate refuses BEFORE the send, so
// RadioModel::sendCmd is never reached and commandDropped() never fires --
// which means a control converted from "drops silently" to "refuses" takes the
// operator's only feedback away with it unless the gate speaks. See the comment
// on MainWindow::showUnsupportedControlNotice() in MainWindow_Session.cpp,
// which is why the two existing gates (split_toggle in MainWindow_Shortcuts.cpp
// and the VfoWidget::splitToggled lambda in MainWindow_Wiring.cpp) pair
// qCWarning(lcDevices) with that notice. Both call sites here do the same, on
// the NotDeclared rung and only on that rung: that is the rung that replaces
// the #5263 loud drop, while NotConnected and NoPan replace early returns that
// were silent before and stay silent now. panZoomModeRefusal() exists so a
// caller can tell those apart; a caller that only needs yes/no asks
// panZoomModeWritable().
//
// So the availability the buttons show and the admissibility the command paths
// test are now the SAME function rather than two copies of one condition.
// bandSegmentZoomAvailable() is panZoomModeWritable() with a pan present --
// asserted rather than asserted-by-comment, so the enable state and the write
// decision cannot drift apart on the capability. A future entry point that
// forgets to call this is still a bug; two entry points quietly disagreeing
// about what "available" means no longer is.
//
// RECEIVE-ONLY BY CONSTRUCTION, and this was checked rather than assumed. The
// only thing either refused path can emit is
// `display pan set <panId> band_zoom=<0|1>` (or `segment_zoom=`), built in
// MainWindow::togglePanZoomModeForPan and MainWindow::setPanZoomMode and handed
// to RadioModel::sendCommand. It is display-domain text: it selects what span
// the panadapter draws. It reaches no TX verb, no keying path, and nothing in
// RadioModel_TxCoordinator; ShortcutManager::Action::keysTx is false at both
// registration sites. Nor can a dropped send invert anything client-side --
// the toggle reads the pan's radio-authoritative flag rather than a latched
// bool (#4057). Bypassing this gate was a dead control, never an emission.
//
// NOT MERGED INTO THE OWNERSHIP GATE in RadioModel::sendCommand. That one is a
// last-line drop for `display pan set ` writes to a pan another client owns; it
// runs after the UI has already acted. The question here is whether the UI
// should act at all, which is what the button gate answers, and answering it in
// two different layers is how the two copies appeared in the first place.
//
// WHY src/gui/ AND NOT src/models/. This is UI policy -- what the UI may send,
// and whether a button is grey -- not engine state. It has no includes, no Qt
// type and no engine type, and its only consumers are MainWindow and one test.
// src/gui/ already holds exactly this shape (DStarAvailabilityGate.h,
// DvkAvailabilityGate.h, DaxRestorePolicy.h,
// BandRecallSliceSelectionPolicy.h), tests include those directly without
// linking the GUI, and keeping it here adds no row to the aetherd engine/UI
// touchpoint manifest -- a burndown that is supposed to go down.

namespace AetherSDR {

// Why a band/segment-zoom write is refused, or None if it is admissible.
// Ordered from the most general refusal to the most specific so a caller that
// wants to explain itself names the outermost reason. NotDeclared is the one
// the caller must announce -- see "A REFUSAL MUST SAY SO" above.
enum class PanZoomModeRefusal {
    None,
    NotConnected,
    NotDeclared,
    NoPan,
};

// `panZoomModesDeclared` is RadioCapabilities::panZoomModes.has_value() off
// RadioModel::backendCapabilities() -- a per-feature record the backend
// engages, NOT a family string. Absent means UNDECLARED, and undeclared
// refuses: the failure it describes is a control that moves while the write is
// dropped.
// `panKnown` is "a non-empty pan id that RadioModel::panadapter() resolves".
[[nodiscard]] constexpr PanZoomModeRefusal panZoomModeRefusal(
    bool connected, bool panZoomModesDeclared, bool panKnown) noexcept
{
    if (!connected) {
        return PanZoomModeRefusal::NotConnected;
    }
    if (!panZoomModesDeclared) {
        return PanZoomModeRefusal::NotDeclared;
    }
    if (!panKnown) {
        return PanZoomModeRefusal::NoPan;
    }
    return PanZoomModeRefusal::None;
}

// May this client write band_zoom=/segment_zoom= for this pan right now?
// Every command path asks this; none of them may ask anything narrower.
[[nodiscard]] constexpr bool panZoomModeWritable(
    bool connected, bool panZoomModesDeclared, bool panKnown) noexcept
{
    return panZoomModeRefusal(connected, panZoomModesDeclared, panKnown)
           == PanZoomModeRefusal::None;
}

// Should the "B"/"S" buttons be enabled? The same question with the pan taken
// as present: a pan applet exists before the radio hands back its id, and the
// buttons are enabled per radio rather than per pan.
[[nodiscard]] constexpr bool bandSegmentZoomAvailable(
    bool connected, bool panZoomModesDeclared) noexcept
{
    return panZoomModeWritable(connected, panZoomModesDeclared,
                               /*panKnown=*/true);
}

}  // namespace AetherSDR
