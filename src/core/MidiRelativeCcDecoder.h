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

// The largest real single-message step is +/-50 (CTR2-MIDI manual v2.01 p35,
// WheelA full speed); a unit detent from the other encoding decoded under a stale
// lock lands at +/-62..63, so a delta >= this means the encoding changed. Only
// steps |d| <= 8 from the other family cross it (they decode as 64 - |d|); Speed
// Tuning bursts (+/-12, +/-24) don't and are healed by the next unit detent.
// Lowering it would false-heal legitimate +/-50 spins (#4299).
constexpr int kMidiRelativeCcImplausibleStep = 56;

// Relative CC has no encoding marker; the unit detents are disjoint
// (two's-complement 1/127, center-64 65/63), so only a single-detent first value
// commits an encoding. Any other first value defers (delta 0, Undetermined): a
// wrong lock would decode every later 63/65 as -/+63 (#4096). A locked encoding
// re-classifies from any decode at or beyond kMidiRelativeCcImplausibleStep
// (deferring if that byte is ambiguous).
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
