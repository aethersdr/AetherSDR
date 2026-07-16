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

    return ok ? 0 : 1;
}
