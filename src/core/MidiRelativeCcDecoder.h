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

// Relative MIDI CC has no encoding marker on the wire. Preserve the established
// two's-complement 1/127 convention unless the first directional value is the
// distinctive 63/65 pair used by center-64 controllers such as CTR2-MIDI.
inline MidiRelativeCcDecodeResult decodeMidiRelativeCc(
    int value, MidiRelativeCcEncoding currentEncoding)
{
    MidiRelativeCcEncoding encoding = currentEncoding;
    if (encoding == MidiRelativeCcEncoding::Undetermined) {
        if (value == 63 || value == 65) {
            encoding = MidiRelativeCcEncoding::Center64;
        } else if (value != 64) {
            encoding = MidiRelativeCcEncoding::TwosComplement;
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
