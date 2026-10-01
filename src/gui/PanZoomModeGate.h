#pragma once

// One predicate for band/segment zoom: whether the "B"/"S" buttons are enabled
// and whether a command path may send `display pan set <panId> band_zoom=` /
// `segment_zoom=` (Flex wire text). Every non-button surface goes through
// MainWindow::togglePanZoomMode / setPanZoomMode. Capability:
// RadioCapabilities::panZoomModes.has_value(), not a family (#5554). On
// NotDeclared callers warn and call showUnsupportedControlNotice();
// NotConnected/NoPan stay silent. Receive-only: no TX path (keysTx false).

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
