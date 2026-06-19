#include "core/MemoryFieldValues.h"

#include <QString>

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

} // namespace

int main()
{
    using namespace AetherSDR::MemoryFields;
    bool ok = true;

    // ── sanitizeText strips NUL and control bytes ───────────────────────────
    {
        QString withNul = "FM";
        withNul.insert(1, QChar(QChar::Null)); // "F\0M"
        ok &= expect(sanitizeText(withNul) == "FM",
                     "sanitizeText removes an embedded NUL from a mode value");
    }
    ok &= expect(sanitizeText(QString("A\tB\nC\rD")) == "ABCD",
                 "sanitizeText removes TAB/LF/CR control bytes");
    ok &= expect(sanitizeText(QString("X") + QChar(0x7f) + "Y") == "XY",
                 "sanitizeText removes the reserved DEL byte");
    ok &= expect(sanitizeText("normal name") == "normal name",
                 "sanitizeText preserves ordinary printable text incl. spaces");

    // ── mode helpers ────────────────────────────────────────────────────────
    ok &= expect(isKnownMode("usb"), "isKnownMode is case-insensitive for USB");
    ok &= expect(isKnownMode("NFM"), "isKnownMode accepts NFM");
    ok &= expect(!isKnownMode("FOO"), "isKnownMode rejects an unknown mode");
    ok &= expect(modeToWire(" digu ") == "DIGU",
                 "modeToWire trims and upper-cases");
    {
        QString dirty = "USB";
        dirty.insert(1, QChar(QChar::Null));
        ok &= expect(modeToWire(dirty) == "USB",
                     "modeToWire also sanitizes control bytes");
    }

    // ── offset direction round-trips ────────────────────────────────────────
    ok &= expect(offsetDirToWire("SIMPLEX") == "simplex", "offsetDirToWire simplex");
    ok &= expect(offsetDirToWire("up") == "up", "offsetDirToWire up");
    ok &= expect(offsetDirToWire("Down") == "down", "offsetDirToWire down");
    ok &= expect(offsetDirToWire("garbage").isEmpty(),
                 "offsetDirToWire empties an unrecognized direction");
    ok &= expect(offsetDirToDisplay("up") == "UP", "offsetDirToDisplay up");
    ok &= expect(offsetDirToDisplay("") == "SIMPLEX",
                 "offsetDirToDisplay defaults blank to SIMPLEX");

    // ── tone mode round-trips ───────────────────────────────────────────────
    ok &= expect(toneModeToWire("CTCSS_TX") == "ctcss_tx", "toneModeToWire ctcss_tx");
    ok &= expect(toneModeToWire("off") == "off", "toneModeToWire off");
    ok &= expect(toneModeToWire("bogus").isEmpty(),
                 "toneModeToWire empties an unrecognized tone mode");
    ok &= expect(toneModeToDisplay("ctcss_tx") == "CTCSS_TX", "toneModeToDisplay ctcss_tx");
    ok &= expect(toneModeToDisplay("") == "OFF",
                 "toneModeToDisplay defaults blank to OFF");

    // ── value lists are populated and well-formed ───────────────────────────
    ok &= expect(modes().contains("USB") && modes().contains("AME"),
                 "modes() spans the FlexLib demod set");
    ok &= expect(offsetDirectionsDisplay().size() == 3,
                 "offsetDirectionsDisplay() has SIMPLEX/UP/DOWN");
    ok &= expect(toneModesDisplay() == QStringList({"OFF", "CTCSS_TX"}),
                 "toneModesDisplay() is OFF then CTCSS_TX");
    ok &= expect(ctcssTones().contains("88.5") && ctcssTones().contains("254.1"),
                 "ctcssTones() includes standard EIA tones");
    ok &= expect(!tuningSteps().isEmpty(), "tuningSteps() is populated");

    std::cerr << (ok ? "memory_field_values_test PASSED\n"
                     : "memory_field_values_test FAILED\n");
    return ok ? 0 : 1;
}
