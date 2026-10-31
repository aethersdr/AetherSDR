#pragma once

// When the Flex-shaped voice controls (PROC, 8-band graphic EQ) may write the
// SHARED ClientComp / ClientEq objects the Aetherial strip also edits (#4609).
// `chainOwned`: has such a control written them in this process, so they hold
// OUR layout? Pure so tests exercise what MainWindow uses.

// A graphic EQ restored from settings (EqualizerModel::State) is not ownership:
// nothing is written until a host-modulating connect re-pushes it. The strip
// persists ClientEq/ClientComp under its own keys, which the graphic EQ never
// writes; PROC state does not persist.

namespace AetherSDR {

// May a connection edge re-push the operator's PROC and graphic-EQ state onto
// the shared objects?
//
// Only onto a backend that modulates on this host — a Flex reaches its own DSP
// through the command plane and would then be equalized twice — and only when
// the operator has put something there. Without the last term this fires on
// every connect and writes EqualizerModel's all-zero default over the operator's
// persisted ClientEq bands, for someone who has never opened the applet.
constexpr bool hostVoiceChainRepushAllowed(bool connected, bool hostModulates,
                                           bool chainOwned) noexcept
{
    return connected && hostModulates && chainOwned;
}

// Must this connection edge take our layout out of circuit? Case: a graphic EQ
// written on an HL2 in this process, moved or re-pushed from settings, then a
// Flex connects; ClientEq would hold the eight filters on top of the radio's EQ.
// `chainOwned` limits it to that: plain Flex connects/disconnects must not disable
// the Aetherial RX/TX EQ and compressor, family-agnostic by design
// (docs/architecture/radio-capabilities-map.md).
constexpr bool hostVoiceChainUnwindRequired(bool connected, bool hostModulates,
                                            bool usesFlexCommandPlane,
                                            bool chainOwned) noexcept
{
    // Still on a host-modulating backend: nothing to unwind, the mapping is
    // exactly where it belongs.
    if (connected && hostModulates)
        return false;
    return chainOwned && usesFlexCommandPlane;
}

// Does a graphic EQ restored from settings hold a layout the next
// host-modulating connect should re-push? Only when it holds something: any band
// off 0 dB, or either enable on. An all-default restore is an operator who never
// used the applet, and their strip layout must not be re-pushed over.
constexpr bool restoredGraphicEqHoldsLayout(bool anyBandNonZero,
                                               bool anyEnabled) noexcept
{
    return anyBandNonZero || anyEnabled;
}

// May the graphic EQ's state be written to settings now? Only while it is the
// client's to keep: connected to a backend that modulates on this host, and the
// model not holding a radio's EQ. `modelFromRadio`: a Flex has written its EQ
// status into EqualizerModel since the saved layout was last loaded into it.
constexpr bool graphicEqPersistAllowed(bool connected, bool hostModulates,
                                       bool modelFromRadio) noexcept
{
    return connected && hostModulates && !modelFromRadio;
}

// Must this connection edge load the saved layout back into the model? On a
// host-modulating connect after a radio wrote the model, before any edit can
// save the radio's values over the operator's saved layout.
constexpr bool graphicEqReloadRequired(bool connected, bool hostModulates,
                                       bool modelFromRadio) noexcept
{
    return connected && hostModulates && modelFromRadio;
}

}  // namespace AetherSDR
