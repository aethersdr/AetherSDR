#include "models/TransmitInhibitPolicy.h"

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

    {
        const auto parsed = TransmitInhibitPolicy::parseSliceTxCommand(
            QStringLiteral("slice set 3 tx=1"));
        ok &= expect(parsed.valid && parsed.sliceId == 3 && parsed.txEnabled,
                     "parses tx enable command");
    }

    {
        const auto parsed = TransmitInhibitPolicy::parseSliceTxCommand(
            QStringLiteral(" slice   set   12   audio_gain=50 tx=1 "));
        ok &= expect(parsed.valid && parsed.sliceId == 12 && parsed.txEnabled,
                     "parses tx enable among other keys");
    }

    {
        const auto parsed = TransmitInhibitPolicy::parseSliceTxCommand(
            QStringLiteral("slice set 3 tx=0"));
        ok &= expect(parsed.valid && parsed.sliceId == 3 && !parsed.txEnabled,
                     "parses tx disable command");
    }

    {
        const auto parsed = TransmitInhibitPolicy::parseSliceTxCommand(
            QStringLiteral("slice set 3 audio_gain=50"));
        ok &= expect(!parsed.valid, "ignores non-TX slice command");
    }

    {
        const auto parsed = TransmitInhibitPolicy::parseSliceTxCommand(
            QStringLiteral("display pan 0x40000000 bandwidth=0.2"));
        ok &= expect(!parsed.valid, "ignores non-slice command");
    }

    return ok ? 0 : 1;
}
