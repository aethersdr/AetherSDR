#pragma once

#include <QString>

namespace AetherSDR {

// Owned configuration for the TX Linearity Analyzer, per Constitution
// Principle V: ONE nested JSON object under a single root key
// ("TxLinearity"), defaulted, read and written as a unit — never scattered as
// loose flat keys across the shared AppSettings namespace.
//
// Shape:
//   {"daxChannel":int, "toneSpacingHz":double, "tunePowerW":int,
//    "averages":int, "timeoutSec":int, "window":string, "maxOrder":int}
//
// There is no legacy-flat-key migration here because this feature has never
// shipped: the object is the only shape its settings have ever had.
//
// WHAT IS DELIBERATELY ABSENT. The dummy-load / verified-clear confirmation is
// NOT persisted and must never be added to this object. §8 requires explicit
// confirmation before every test transmission and requires it not to be
// remembered across runs or sessions — the antenna on the other end of the
// coax is not a preference, it is a fact that can change between one run and
// the next. TxLinearitySafety holds it in memory for exactly one run and
// clears it on stop.
//
// The transmit timeout IS persisted, but restoring it can only ever tighten
// the interlock: TxLinearitySafety::setLimits() refuses a value that loosens a
// limit without an explicit override, so a corrupted or hand-edited stored
// value cannot quietly widen the transmit window.
struct TxLinearitySettings {
    int daxChannel = 1;
    double toneSpacingHz = 2000.0;
    int tunePowerW = 10;
    int averages = 8;
    int timeoutSec = 10;
    QString window = QStringLiteral("blackman-harris-7");
    int maxOrder = 7;

    // Read the whole object, falling back to the defaults above for any field
    // the stored document is missing or has stored with the wrong type. A
    // partially-written or hand-edited document degrades field by field rather
    // than resetting the operator's whole setup.
    static TxLinearitySettings load();

    // Write the whole object atomically under the single root key.
    void save() const;

    // Clamp every field into the range its control accepts. Called by load(),
    // so a stored document that is out of range — from an older build with
    // different limits, or from a hand edit — can never drive a control past
    // its bounds or hand the analyzer an unusable FFT configuration.
    void clampToValidRanges();

    static QString rootKey();   // "TxLinearity"
};

}  // namespace AetherSDR
