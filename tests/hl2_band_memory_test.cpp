// Per-band LNA memory across a connect that pins the gain.
//
// The defect these cover is a SILENT, DELAYED loss of operator calibration.
// A connect carrying the namespaced lnaGainDb param sets the live gain to the
// pinned value while the start band's stored entry says something else; the
// first band change then calls rememberCurrentBandState(), which writes the
// live value back over that entry. Nothing is wrong at connect, nothing warns,
// and the band that used to be calibrated comes up on the pinned value in every
// later session as though the operator had chosen it.
//
// This station lost 20 m that way during a witnessed run on 2026-09-03: the
// stored -12 dB entry was replaced by the launch's pinned value, and the loss
// was only visible because a backup had been taken minutes earlier.
//
// Hl2Backend evaluates these same functions rather than its own copy, so what
// passes here is what the radio runs
// (core/backends/hl2/Hl2BandMemoryPolicy.h).

#include "core/backends/hl2/Hl2BandMemoryPolicy.h"

#include <cstdio>

using AetherSDR::hl2::bandMemoryWriteback;
using AetherSDR::hl2::connectLna;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

// This station's clamp, from Hl2Backend's kLnaGainMinDb/kLnaGainMaxDb.
constexpr int kMin = -12;
constexpr int kMax = 48;
constexpr int kDefault = 20;

}  // namespace

int main()
{
    // ---- A connect with no param takes the band's stored entry -------------
    //
    // Already true before this header existed. Kept because it is the
    // precondition for everything below: if a plain connect did NOT restore the
    // stored entry, the writeback case would be unreachable and the defect
    // would be somewhere else entirely.
    {
        const auto seed = connectLna(/*haveRestoredState=*/true,
                                     /*hasStoredEntry=*/true, /*storedDb=*/-12,
                                     /*paramPresent=*/false, /*paramDb=*/0,
                                     kDefault, kMin, kMax);
        check(seed.liveDb == -12,
              "a plain connect comes up on the start band's stored entry");
        check(!seed.sessionPin,
              "and nothing about that value is a session pin");
    }

    // ---- A connect WITH the param pins the live value ----------------------
    //
    // The param still wins, deliberately: it is how an automation or test
    // caller pins the gain, and this fix does not reverse that precedence.
    // What it does is mark the divergence, because the divergence is what the
    // band memory must not swallow.
    {
        const auto seed = connectLna(/*haveRestoredState=*/true,
                                     /*hasStoredEntry=*/true, /*storedDb=*/-12,
                                     /*paramPresent=*/true, /*paramDb=*/20,
                                     kDefault, kMin, kMax);
        check(seed.liveDb == 20,
              "an explicit lnaGainDb param still wins the live value");
        check(seed.sessionPin,
              "and is marked a session pin, because the band stored -12");
    }

    // ---- THE DEFECT: the first band change must not consume the entry ------
    {
        const auto seed = connectLna(true, true, -12, true, 20, kDefault, kMin, kMax);
        const int written = bandMemoryWriteback(seed.liveDb, seed.sessionPin,
                                                /*hasStoredEntry=*/true,
                                                /*storedDb=*/-12);
        check(written == -12,
              "leaving the start band after a pinned connect KEEPS the stored -12");
    }

    // ---- A pin that agrees with the entry is not a pin ---------------------
    //
    // Pinning the value the band already had costs nothing and must not be
    // treated as a divergence — otherwise the flag is set on ordinary
    // automation connects and stops meaning anything.
    {
        const auto seed = connectLna(true, true, -12, true, -12, kDefault, kMin, kMax);
        check(!seed.sessionPin,
              "a param equal to the stored entry is not a session pin");
        check(bandMemoryWriteback(seed.liveDb, seed.sessionPin, true, -12) == -12,
              "and records the same -12 either way");
    }

    // ---- An operator value on a band with no entry is still recorded -------
    //
    // The fix must not turn the memory off. A band the operator has never
    // calibrated has nothing to protect, so the live value is what gets stored
    // — including when it arrived as a connect param.
    {
        const auto seed = connectLna(true, /*hasStoredEntry=*/false, 0,
                                     /*paramPresent=*/true, /*paramDb=*/6,
                                     kDefault, kMin, kMax);
        check(!seed.sessionPin,
              "a param on an uncalibrated band is not a pin — nothing to lose");
        check(bandMemoryWriteback(seed.liveDb, seed.sessionPin, false, 0) == 6,
              "and leaving that band records it, so the memory still works");
    }

    // ---- Ordinary operation is untouched ----------------------------------
    {
        // No pin at all: the operator moved the slider to +30 on a band that
        // remembered -12. That is real intent and must overwrite.
        check(bandMemoryWriteback(/*liveDb=*/30, /*sessionPin=*/false,
                                  /*hasStoredEntry=*/true, /*storedDb=*/-12) == 30,
              "without a pin, the live value overwrites the entry as before");
    }

    // ---- A stored entry outside the clamp is bounded, not honoured ---------
    {
        const auto seed = connectLna(true, true, /*storedDb=*/900,
                                     false, 0, kDefault, kMin, kMax);
        check(seed.liveDb == kMax,
              "a stored entry above the range clamps to the ceiling");
    }

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
