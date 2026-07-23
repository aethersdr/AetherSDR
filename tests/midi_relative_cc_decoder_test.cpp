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

    return ok ? 0 : 1;
}
