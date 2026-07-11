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

    {
        ok &= expect(TransmitInhibitPolicy::shouldRestoreInhibitedTxSlice(
                         QStringLiteral("0x40000000"),
                         QStringLiteral("0x40000000"), true, 3, -1),
                     "restores inhibited TX slice when radio has no current TX slice");
    }

    {
        ok &= expect(TransmitInhibitPolicy::shouldRestoreInhibitedTxSlice(
                         QStringLiteral("0x40000000"),
                         QStringLiteral("0x40000000"), true, 3, 3),
                     "restores inhibited TX slice when it remains current TX");
    }

    {
        ok &= expect(!TransmitInhibitPolicy::shouldRestoreInhibitedTxSlice(
                         QStringLiteral("0x40000000"),
                         QStringLiteral("0x40000000"), true, 3, 4),
                     "does not restore when TX moved to another slice");
    }

    {
        ok &= expect(!TransmitInhibitPolicy::shouldRestoreInhibitedTxSlice(
                         QStringLiteral("0x40000000"),
                         QStringLiteral("0x40000001"), true, 3, -1),
                     "does not restore a slice on another panadapter");
    }

    {
        ok &= expect(!TransmitInhibitPolicy::shouldRestoreInhibitedTxSlice(
                         QStringLiteral("0x40000000"),
                         QStringLiteral("0x40000000"), false, 3, -1),
                     "does not restore a slice we cannot safely claim");
    }

    return ok ? 0 : 1;
}
