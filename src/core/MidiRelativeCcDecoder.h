#pragma once

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

// Relative MIDI CC has no encoding marker on the wire. The two encodings' unit
// detents are disjoint — two's-complement sends 1 (CW) / 127 (CCW), center-64
// sends 65 (CW) / 63 (CCW) — so a SINGLE-detent first value is an unambiguous
// discriminator. Any other first value (e.g. a fast first turn landing on 70)
// is ambiguous and must NOT commit an encoding: locking on it would, for a
// center-64 controller whose first move isn't a unit detent, wrongly select
// two's-complement and then decode every later 63/65 detent as ∓63 — the #4096
// jumps, made permanent. Defer instead (delta 0, stay Undetermined) until a
// unit detent settles the encoding.
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

    if (encoding == MidiRelativeCcEncoding::Center64) {
        return {value - 64, encoding};
    }
    if (encoding == MidiRelativeCcEncoding::TwosComplement) {
        return {(value < 64) ? value : (value - 128), encoding};
    }
    return {0, encoding};
}

} // namespace AetherSDR
