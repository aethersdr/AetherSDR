#include "core/MidiRelativeCcDecoder.h"

#include <iostream>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

} // namespace

int main()
{
    bool ok = true;

    MidiRelativeCcEncoding encoding = MidiRelativeCcEncoding::Undetermined;
    MidiRelativeCcDecodeResult result = decodeMidiRelativeCc(65, encoding);
    encoding = result.encoding;
    ok &= expect(result.delta == 1, "CTR2 clockwise detent is one step");
    ok &= expect(encoding == MidiRelativeCcEncoding::Center64,
                 "CTR2 detent selects center-64 encoding");

    result = decodeMidiRelativeCc(63, encoding);
    ok &= expect(result.delta == -1, "CTR2 counter-clockwise detent is one step");

    encoding = MidiRelativeCcEncoding::Undetermined;
    result = decodeMidiRelativeCc(1, encoding);
    encoding = result.encoding;
    ok &= expect(result.delta == 1, "two's-complement clockwise pulse is preserved");
    ok &= expect(encoding == MidiRelativeCcEncoding::TwosComplement,
                 "unit pulse selects two's-complement encoding");

    result = decodeMidiRelativeCc(127, encoding);
    ok &= expect(result.delta == -1,
                 "two's-complement counter-clockwise pulse is preserved");

    encoding = MidiRelativeCcEncoding::Undetermined;
    result = decodeMidiRelativeCc(64, encoding);
    ok &= expect(result.delta == 0, "center value is neutral before detection");
    ok &= expect(result.encoding == MidiRelativeCcEncoding::Undetermined,
                 "neutral value does not lock an encoding");

    // #4096 regression guard: an ambiguous FIRST value (a fast first turn that
    // isn't a unit detent, e.g. 70) must NOT lock an encoding. Locking it as
    // two's-complement would decode every later center-64 detent as ∓63.
    encoding = MidiRelativeCcEncoding::Undetermined;
    result = decodeMidiRelativeCc(70, encoding);
    encoding = result.encoding;
    ok &= expect(result.delta == 0, "ambiguous first value yields no step");
    ok &= expect(encoding == MidiRelativeCcEncoding::Undetermined,
                 "ambiguous first value does not lock an encoding");
    // The first real unit detent then settles it correctly (center-64), and
    // subsequent detents decode as ±1 rather than the mis-locked ∓63.
    result = decodeMidiRelativeCc(65, encoding);
    encoding = result.encoding;
    ok &= expect(result.delta == 1 && encoding == MidiRelativeCcEncoding::Center64,
                 "first real detent after ambiguity selects center-64 (+1)");
    result = decodeMidiRelativeCc(63, encoding);
    ok &= expect(result.delta == -1, "and the opposite detent is one step, not -63");

    // A two's-complement controller is likewise unaffected: its unit pulse
    // still locks two's-complement even after an ambiguous first sample.
    encoding = MidiRelativeCcEncoding::Undetermined;
    result = decodeMidiRelativeCc(10, encoding);   // ambiguous (fast first turn)
    encoding = result.encoding;
    ok &= expect(result.delta == 0 && encoding == MidiRelativeCcEncoding::Undetermined,
                 "ambiguous first value defers for two's-complement too");
    result = decodeMidiRelativeCc(1, encoding);
    ok &= expect(result.delta == 1
                 && result.encoding == MidiRelativeCcEncoding::TwosComplement,
                 "unit pulse still selects two's-complement after ambiguity");

    // Stale-lock self-heal: the controller's knob mode changed mid-session, so the
    // locked encoding decodes the other wheel family's bytes to implausible steps.
    encoding = MidiRelativeCcEncoding::Center64;
    result = decodeMidiRelativeCc(1, encoding);
    encoding = result.encoding;
    ok &= expect(result.delta == 1,
                 "two's-complement pulse under stale center-64 lock heals to one step");
    ok &= expect(encoding == MidiRelativeCcEncoding::TwosComplement,
                 "healed lock re-pins to two's-complement");
    result = decodeMidiRelativeCc(126, encoding);
    ok &= expect(result.delta == -2,
                 "healed two's-complement decode preserves counter-clockwise value");

    encoding = MidiRelativeCcEncoding::TwosComplement;
    result = decodeMidiRelativeCc(65, encoding);
    encoding = result.encoding;
    ok &= expect(result.delta == 1,
                 "center-64 detent under stale two's-complement lock heals to one step");
    ok &= expect(encoding == MidiRelativeCcEncoding::Center64,
                 "healed lock re-pins to center-64");
    result = decodeMidiRelativeCc(63, encoding);
    ok &= expect(result.delta == -1,
                 "healed center-64 decode is symmetric counter-clockwise");

    // No false heal on real spins: full-speed center-64 batches reach +/-50
    // (CTR2-MIDI manual p35); the lock must survive them.
    encoding = MidiRelativeCcEncoding::Center64;
    result = decodeMidiRelativeCc(114, encoding);
    ok &= expect(result.delta == 50, "spec-max clockwise spin decodes under the lock");
    ok &= expect(result.encoding == MidiRelativeCcEncoding::Center64,
                 "spec-max clockwise spin does not flip the lock");
    result = decodeMidiRelativeCc(14, encoding);
    ok &= expect(result.delta == -50,
                 "spec-max counter-clockwise spin decodes under the lock");
    result = decodeMidiRelativeCc(9, encoding);
    ok &= expect(result.delta == -55, "delta below the threshold never re-classifies");

    // At or beyond the threshold the heal re-classifies from the same byte; an
    // ambiguous byte defers (like any first sample) until a unit detent settles it.
    result = decodeMidiRelativeCc(120, encoding);
    ok &= expect(result.delta == 0
                     && result.encoding == MidiRelativeCcEncoding::Undetermined,
                 "threshold heal on an ambiguous byte defers re-detection");
    result = decodeMidiRelativeCc(127, result.encoding);
    ok &= expect(result.delta == -1
                     && result.encoding == MidiRelativeCcEncoding::TwosComplement,
                 "unit detent after a deferred heal settles the encoding");

    // The heal inherits the deferral for non-unit direction tokens too: a stale
    // center-64 lock fed WheelB counter-clockwise (126 -> +62) unlocks rather
    // than guessing an encoding from an ambiguous byte.
    encoding = MidiRelativeCcEncoding::Center64;
    result = decodeMidiRelativeCc(126, encoding);
    ok &= expect(result.delta == 0
                     && result.encoding == MidiRelativeCcEncoding::Undetermined,
                 "stale-lock heal on a non-unit byte unlocks without guessing");

    // Documented coverage limit of the one-message heal (see
    // kMidiRelativeCcImplausibleStep). A step |d| from the switched wheel decodes
    // under the stale lock to magnitude 64 - |d|, so only |d| <= 8 crosses the
    // threshold. Speed Tuning bursts (+/-12 Normal, +/-24 Fast on the reference
    // unit) land below it and are bit-identical to a legitimate spin of the
    // locked encoding, so they decode under the stale lock and it survives.
    // Pinned so anyone retuning the threshold sees the trade-off: catching these
    // would false-heal legitimate spec-max (+/-50) spins.
    encoding = MidiRelativeCcEncoding::Center64;
    result = decodeMidiRelativeCc(12, encoding); // two's-complement +12 after a switch
    ok &= expect(result.delta == -52
                     && result.encoding == MidiRelativeCcEncoding::Center64,
                 "Speed-Tuning-sized step from the switched wheel misses the heal");
    result = decodeMidiRelativeCc(24, encoding); // two's-complement +24 after a switch
    ok &= expect(result.delta == -40
                     && result.encoding == MidiRelativeCcEncoding::Center64,
                 "fast-spin step from the switched wheel also misses the heal");
    // Recovery path: the next unit detent still heals, whatever came before it.
    result = decodeMidiRelativeCc(1, encoding);
    ok &= expect(result.delta == 1
                     && result.encoding == MidiRelativeCcEncoding::TwosComplement,
                 "a unit detent heals even after missed Speed Tuning bursts");

    return ok ? 0 : 1;
}
