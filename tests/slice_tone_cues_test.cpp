// Regression guard for #5097.
//
// The panadapter used to treat DIGL as if it were RTTY and draw mark/space tone
// cues *instead of* the carrier marker, so every DIGL slice — FT8, JS8, PSK31,
// VARA, SSTV and RADE on any lower-sideband band — lost its centre line and
// triangle and gained two meaningless FSK lines at 2125 / 2295 Hz.
//
// DIGL was added to the RTTY branch by 8fba0f97 (PR #660). Removing it lets a
// DIGL slice fall through to the normal carrier path in all three renderers
// (2D painter, GPU shadow cues, 3D CPU fallback). RTTY rendering is unchanged:
// there the RF frequency IS the mark, so replacing the carrier is correct.
//
// This test pins the predicate. The carrier-restored half of the symptom lives
// inside the three renderers and is not extractable without factoring out cue
// selection as a pure function — see the note on the PR.

#include "gui/SliceToneCues.h"

#include <cstdio>

using AetherSDR::drawsRttyToneCues;

namespace {

int g_failures = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failures;
    }
}

void expectCues(const char* mode, bool expected)
{
    const bool actual = drawsRttyToneCues(QString::fromLatin1(mode));
    char name[96];
    std::snprintf(name, sizeof(name), "%s %s tone cues", mode,
                  expected ? "draws" : "does not draw");
    report(name, actual == expected);
}

} // namespace

int main()
{
    // The only mode that draws them.
    expectCues("RTTY", true);

    // The regression this test exists for. DIGL is a general LSB *data* mode,
    // not an FSK mode; it must keep its carrier marker and gain no tone cues.
    expectCues("DIGL", false);

    // Every other mode the radio advertises (FLEX-8400, fw 4.2.20.41343).
    for (const char* mode : {"LSB", "USB", "AM", "CW", "DIGU", "SAM",
                             "FM", "NFM", "DFM", "FDVU", "FDVL"}) {
        expectCues(mode, false);
    }

    // Mode strings arrive from the radio in canonical upper case (FlexLib
    // `slice status mode=`), so matching is deliberately exact — a lower-case
    // spelling is not a mode this client ever sees, and silently accepting one
    // would hide a wire-format change rather than surface it.
    report("matching is case-sensitive",
           !drawsRttyToneCues(QStringLiteral("rtty")));

    // Defensive: an empty / absent mode must not enable tone cues.
    report("empty mode draws no tone cues",
           !drawsRttyToneCues(QString()));

    if (g_failures == 0) {
        std::printf("All slice tone-cue checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
