#pragma once

// When the Flex-shaped voice controls (PROC, 8-band graphic EQ) may write the
// SHARED ClientComp / ClientEq objects the Aetherial strip also edits (one object,
// two surfaces, so nothing is double-processed; #4609). The predicates turn on
// `chainOwned`: do those controls hold a layout of the operator's own, so the
// shared objects are OURS to write? Pure so tests exercise what MainWindow uses.
//
// Ownership has two sources. The operator moves a Flex-shaped control in this
// process; or a graphic EQ comes back from settings (EqualizerModel::State) with
// something in it. TransmitModel's PROC state does not persist; ClientEq and
// ClientComp persist under the strip's own keys, which the graphic EQ never writes.

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
// set on an HL2, in this process or restored from an earlier one, then a Flex
// connects; ClientEq would hold the eight filters on top of the radio's EQ.
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

// Does a graphic EQ restored from settings claim the shared objects at startup?
// Only when it holds something: any band off 0 dB, or either enable on. An
// all-default restore is an operator who never used the applet, and their strip
// layout must not be re-pushed over or unwound.
constexpr bool hostVoiceChainOwnedByRestoredEq(bool anyBandNonZero,
                                               bool anyEnabled) noexcept
{
    return anyBandNonZero || anyEnabled;
}

// May the graphic EQ's state be written to settings now? Only while it is the
// client's to keep: connected to a backend that modulates on this host, the same
// terms the re-push needs. A Flex's EQ lives in the radio, and a value saved from
// a Flex session would claim the shared objects on the next start.
constexpr bool graphicEqPersistAllowed(bool connected, bool hostModulates) noexcept
{
    return connected && hostModulates;
}

}  // namespace AetherSDR
