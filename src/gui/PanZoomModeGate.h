#pragma once

// ONE predicate for band/segment zoom: both whether the "B"/"S" buttons are
// enabled and whether a command path may send.
//
// `band_zoom=`/`segment_zoom=` are FlexLib wire text (togglePanZoomModeForPan),
// dropped by RadioModel::sendCmd on a backend with no command plane. Seven
// operator surfaces reach them: the two waterfall-corner buttons, the
// band_zoom/segment_zoom shortcuts, MIDI global.bandZoom/segmentZoom, the
// FlexControl button table, the RC28/Stream Deck/T-Mate2 chain
// (dispatchHidAction; T-Mate2 key 3 defaults to BandZoom), the FlexControl/
// HID/Ulanzi wheel (applyFlexControlWheelAction), and the bridge `shortcut`
// verb. All but the buttons fan into MainWindow::togglePanZoomMode and
// setPanZoomMode, which call this, so gating those two covers them.
//
// The capability rung is RadioCapabilities::panZoomModes.has_value() (a
// per-feature record, engaged by FlexBackend today), not a family string
// (#5554); a second family engages the record with no edit here.
//
// A refusal must say so: a pre-send gate never reaches commandDropped(), so on
// the NotDeclared rung callers pair qCWarning(lcDevices) with
// showUnsupportedControlNotice(). NotConnected and NoPan stay silent, as
// before. panZoomModeRefusal() distinguishes the rungs; panZoomModeWritable()
// is the yes/no form, and bandSegmentZoomAvailable() is it with a pan present.
//
// Receive-only: the only emission is `display pan set <panId> band_zoom=<0|1>`
// (or segment_zoom), display-domain text with no TX path (keysTx false at
// both shortcut registrations), and the toggle reads the pan's
// radio-authoritative flag (#4057). Separate from the pan-ownership drop in
// RadioModel::sendCommand, which is a last line after the UI acted.
//
// In src/gui/ because it is UI policy with no Qt or engine types, alongside
// DStarAvailabilityGate.h and DaxRestorePolicy.h, adding nothing to the aetherd
// touchpoint manifest.

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
