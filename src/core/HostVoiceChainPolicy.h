#pragma once

// When the Flex-shaped voice controls may touch the SHARED client DSP.
//
// PROC and the 8-band graphic EQ drive the same ClientComp / ClientEq objects
// the Aetherial strip edits — two surfaces onto one object, chosen deliberately
// over private second instances so an operator cannot end up double-processed
// without either UI admitting it (#4609). The cost of that choice is that every
// write from this side lands on settings the operator may have dialled in on
// the other side, so "may we write" is a real question with a wrong answer.
//
// Both predicates below turn on one bit — `chainOwned`: has the operator
// actually moved one of the Flex-shaped controls in this process, so that the
// shared objects are currently holding OUR layout rather than the strip's? It
// is the only thing that distinguishes "re-apply the operator's own choice"
// from "overwrite work this code never made".
//
// The bit cannot come from persistence. EqualizerModel and TransmitModel have
// none — their state arrives from a Flex `eq` / `transmit` status or from an
// operator move, and on a host-modulating backend there is no such status — so
// at a connect edge they sit at their construction defaults: eight EQ bands at
// 0 dB with both enables false, and the speech processor off. ClientEq and
// ClientComp, by contrast, DO persist. Treating the models as authoritative
// there writes defaults over saved state.
//
// Pure functions in a header so the test suite exercises the same expressions
// MainWindow does; the GUI link is not a test target (see
// tests/radio_capability_gating_test.cpp's note on the same limitation).

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

// Must this connection edge take our layout out of circuit?
//
// The case it exists for: move the graphic EQ on an HL2, disconnect, connect a
// Flex in the same process. Every apply now returns early because the family
// uses the Flex command plane, but ClientEq still holds the eight peaking
// filters and is still enabled — so the radio's hardware EQ and ours both
// apply. That is the double-equalization the design note says cannot happen,
// displaced in time rather than in one slider movement.
//
// `chainOwned` is what keeps it to that case. hostModulates is false for a Flex,
// so a plain Flex connect — and every Flex disconnect — reaches this predicate
// too, and without the term it would disable the operator's Aetherial RX EQ, TX
// EQ and TX compressor on a session that never went near an HL2. Those tiles are
// family-agnostic by design: docs/architecture/radio-capabilities-map.md says
// they must NOT be gated, because on a radio with no hardware DSP they are the
// only audio DSP the operator has, and a Flex's receive audio runs through
// ClientEq in AudioEngine::processMixedRxAudioData like every other family's.
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
