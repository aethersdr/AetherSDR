#pragma once

// WHEN AN ANTENNA MENU HAS NOTHING REAL TO OFFER.
//
// The RX and TX antenna buttons (VfoWidget, RxApplet) and the overlay's RX ANT
// combo (SpectrumOverlayMenu) build their choices from what the radio
// publishes -- a slice's rxant/txant list, the panadapter's ant_list -- and,
// when that is empty, FALL BACK TO "ANT1"/"ANT2" so the menu is never blank.
// On a Flex the fallback is a placeholder for a list that is about to arrive.
// On a Hermes-Lite 2 no list ever arrives: the HL2 has one antenna port,
// Hl2Backend publishes none, and IRadioBackend::setSliceRxAntenna() is the base
// no-op. So the operator was offered two ports the radio does not have, picked
// ANT2, watched the label change to ANT2, and nothing moved -- a control that
// looks like it succeeded. The TX pick is worse off still: SliceModel's
// txant= wire text never leaves a non-Flex slice at all.
//
// The rule these predicates encode: a choice is REFUSED -- and the refusal is
// announced through MainWindow::showUnsupportedControlNotice(), not swallowed
// -- when the connected radio published no port to choose, and there is no
// other real destination on offer. For RX the other real destination is a
// KiwiSDR virtual antenna: that one works on every radio (the audio and
// waterfall are client-side streams), so a menu holding Kiwi receivers still
// opens.
//
// "Published" means the radio's own lists -- SliceModel::rxAntennaList() /
// txAntennaList() and RadioModel::antennaList() -- and deliberately NOT
// RadioModel::knownAntennaTokens(), which also folds in the slice's CURRENT
// token and the operator's alias table: a fabricated "ANT2" picked once would
// otherwise vouch for itself forever after.
//
// Disconnected is never refused: nothing has been published yet and the
// widgets keep their old behaviour there.
//
// Pure, no Qt type, so the test includes it directly (PanZoomModeGate.h shape).

namespace AetherSDR {

[[nodiscard]] constexpr bool rxAntennaChoiceRefused(
    bool connected, bool radioPublishedRxPorts, bool hasVirtualAntennas) noexcept
{
    return connected && !radioPublishedRxPorts && !hasVirtualAntennas;
}

[[nodiscard]] constexpr bool txAntennaChoiceRefused(
    bool connected, bool radioPublishedTxPorts) noexcept
{
    return connected && !radioPublishedTxPorts;
}

}  // namespace AetherSDR
