#pragma once

#include <cstdlib>

namespace AetherSDR {

enum class MidiRelativeCcEncoding {
    Undetermined,
    TwosComplement,
    Center64,
};

struct MidiRelativeCcDecodeResult {
    int delta{0};
    MidiRelativeCcEncoding encoding{MidiRelativeCcEncoding::Undetermined};
};

// Largest single-message step a real spin produces is +/-50 (CTR2-MIDI manual v2.01
// p35: WheelA at full speed sends +/-50 counts from center). A *unit detent* from
// the other wheel family decoded under a stale lock lands at +/-62..63, so a delta
// at or beyond this threshold can only mean the controller's encoding changed
// mid-session.
//
// Coverage limit — the heal is deliberately one-message and detent-shaped. A byte
// carrying step |d| from the other family decodes under the stale lock to
// magnitude 64 - |d|, so it only crosses this threshold while |d| <= 8. A Speed
// Tuning burst from the switched wheel (measured +/-12 Normal, +/-24 Fast) lands
// below it and is bit-identical to a legitimate spin of the locked encoding
// (two's-complement +12 == center-64 -52), so it decodes under the stale lock and
// the lock survives; recovery is the next unit detent, which always heals.
// Lowering the threshold to catch those bursts would false-heal legitimate
// spec-max (+/-50) spins and flip a correct lock — strictly worse. Separating them
// needs cross-message state in MidiControlManager, out of scope here (#4299).
constexpr int kMidiRelativeCcImplausibleStep = 56;

// Relative MIDI CC has no encoding marker on the wire. The two encodings' unit
// detents are disjoint — two's-complement sends 1 (CW) / 127 (CCW), center-64
// sends 65 (CW) / 63 (CCW) — so a SINGLE-detent first value is an unambiguous
// discriminator. Any other first value (e.g. a fast first turn landing on 70)
// is ambiguous and must NOT commit an encoding: locking on it would, for a
// center-64 controller whose first move isn't a unit detent, wrongly select
// two's-complement and then decode every later 63/65 detent as ∓63 — the #4096
// jumps, made permanent. Defer instead (delta 0, stay Undetermined) until a
// unit detent settles the encoding.
// A locked encoding self-heals: a decode at or beyond the implausible-step
// threshold means the controller's knob mode changed mid-session, so re-classify
// from that same byte — which, like any first sample, defers if it is ambiguous.
// That covers the unit detent a mode switch is first noticed by; see
// kMidiRelativeCcImplausibleStep for the steps it deliberately does not catch.
inline MidiRelativeCcDecodeResult decodeMidiRelativeCc(
    int value, MidiRelativeCcEncoding currentEncoding)
{
    MidiRelativeCcEncoding encoding = currentEncoding;
    if (encoding == MidiRelativeCcEncoding::Undetermined) {
        if (value == 63 || value == 65) {
            encoding = MidiRelativeCcEncoding::Center64;
        } else if (value == 1 || value == 127) {
            encoding = MidiRelativeCcEncoding::TwosComplement;
        } else {
            return {0, encoding};  // ambiguous first sample — stay Undetermined
        }
    }

    MidiRelativeCcDecodeResult result{0, encoding};
    if (encoding == MidiRelativeCcEncoding::Center64) {
        result.delta = value - 64;
    } else if (encoding == MidiRelativeCcEncoding::TwosComplement) {
        result.delta = (value < 64) ? value : (value - 128);
    }

    if (currentEncoding != MidiRelativeCcEncoding::Undetermined
        && std::abs(result.delta) >= kMidiRelativeCcImplausibleStep) {
        return decodeMidiRelativeCc(value, MidiRelativeCcEncoding::Undetermined);
    }
    return result;
}

} // namespace AetherSDR
