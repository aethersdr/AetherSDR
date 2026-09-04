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
// NO FIELD OBSERVATION IS CLAIMED FOR THIS MECHANISM. An earlier version of
// this comment cited a bench run in which 40 m went from -6 dB to -12 dB. That
// loss is real but it is NOT this defect: neither launch supplied a
// lnaGainDb connect param, so no session pin existed and this path never fired.
// The cause was a separate global RF-gain replay. Inference presented as
// observation, corrected in the PR body and left corrected here. (#5402 review.)
//
// The defect below is established by reading the path and by these assertions.
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
    if (!ok) {
        ++g_failures;
    }
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


    // ---- THE SNAPSHOT PATH, which the write-back protection alone missed ----
    //
    // Hl2Backend::currentOperatingState() builds the persisted band map, and it
    // runs on a DEBOUNCED store that any unrelated action schedules -- a
    // same-band tune, a mode change, a filter change. So it reaches the map long
    // before the first band change, and protecting only rememberCurrentBandState()
    // left the pin free to be persisted through it. (#5402 review, Ozy311.)
    //
    // Both call sites ask THIS function, so these cases cover the production
    // snapshot decision rather than a re-typed copy of it.
    {
        // The reviewer's exact scenario: 20 m stored at -12, connect pins 20,
        // then a same-band tune triggers a capture. The capture must record -12.
        const auto seed = connectLna(/*haveRestoredState=*/true,
                                     /*hasStoredEntry=*/true, /*storedDb=*/-12,
                                     /*paramPresent=*/true, /*paramDb=*/20,
                                     kDefault, kMin, kMax);
        check(seed.liveDb == 20 && seed.sessionPin,
              "snapshot: the pin is live at 20 and marked");
        check(bandMemoryWriteback(seed.liveDb, seed.sessionPin,
                                  /*hasStoredEntry=*/true, /*storedDb=*/-12) == -12,
              "snapshot: a capture during a pinned session records the stored -12");
    }
    {
        // Without a pin the snapshot must still record the live value, or a
        // capture would freeze the band memory against genuine operator changes.
        check(bandMemoryWriteback(/*liveDb=*/30, /*sessionPin=*/false,
                                  /*hasStoredEntry=*/true, /*storedDb=*/-12) == 30,
              "snapshot: without a pin the capture records the live value");
    }
    {
        // A pinned session on a band with NO stored entry has nothing to
        // protect, so the capture records the live value and the memory still
        // learns the band.
        check(bandMemoryWriteback(/*liveDb=*/6, /*sessionPin=*/false,
                                  /*hasStoredEntry=*/false, /*storedDb=*/0) == 6,
              "snapshot: an uncalibrated band still records through a capture");
    }

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
