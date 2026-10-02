#pragma once

// When the Flex-shaped voice controls (PROC, 8-band graphic EQ) may write the
// SHARED ClientComp / ClientEq objects the Aetherial strip also edits (one object,
// two surfaces, so nothing is double-processed; #4609). Both predicates turn on
// `chainOwned`: has the operator moved a Flex-shaped control in this process,
// so the shared objects hold OUR layout? It can't come from persistence:
// EqualizerModel/TransmitModel only get state from Flex status or operator moves
// and sit at defaults at connect, while ClientEq/ClientComp DO persist. Pure so
// tests exercise the expressions MainWindow uses.

namespace AetherSDR {

// May a connection edge re-push the operator's PROC and graphic-EQ state onto
// the shared objects?
//
// Only onto a backend that modulates on this host — a Flex reaches its own DSP
// through the command plane and would then be equalized twice — and only when
// the operator has already put something there. Without the last term this
// fires on every connect and writes EqualizerModel's all-zero construction
// default over the operator's persisted ClientEq bands, for someone who has
// never opened the applet.
constexpr bool hostVoiceChainRepushAllowed(bool connected, bool hostModulates,
                                           bool chainOwned) noexcept
{
    return connected && hostModulates && chainOwned;
}

// Must this connection edge take our layout out of circuit? Case: graphic EQ
// moved on an HL2, then a Flex connected in the same process; ClientEq would still
// hold the eight filters on top of the radio's EQ. `chainOwned` limits it to that
// case: plain Flex connects/disconnects must not disable the Aetherial RX/TX EQ and
// compressor, which are family-agnostic by design
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

}  // namespace AetherSDR
