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
using AetherSDR::hl2::ConnectLna;
using AetherSDR::hl2::migrateStoredLnaDb;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

// THE REAL CONSTANTS, not a hand-typed copy of them. This file used to carry
// `kMax = 48` and `kDefault = 20` as its own literals and went on passing when
// the backend's ceiling moved to +19 -- exercising a range the hardware no
// longer has, and agreeing with itself while the clamp under test was never
// reached. That is the failure Hl2BandMemoryPolicy.h's opening paragraph names,
// committed by the test written to prevent it. (#5752 review, aethersdr-agent.)
using AetherSDR::hl2::kLnaGainMaxDb;
using AetherSDR::hl2::kLnaGainMinDb;
constexpr int kMin = kLnaGainMinDb;
constexpr int kMax = kLnaGainMaxDb;
constexpr int kDefault = AetherSDR::hl2::kLnaDefaultGainDb;

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
        // 18, not 20: the param has to name a gain the radio can apply, or this
        // case asserts the clamp rather than the precedence it is about.
        const auto seed = connectLna(/*haveRestoredState=*/true,
                                     /*hasStoredEntry=*/true, /*storedDb=*/-12,
                                     /*paramPresent=*/true, /*paramDb=*/18,
                                     kDefault, kMin, kMax);
        check(seed.liveDb == 18,
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
    // These cases cover the shared policy. hl2_gain_restore_test separately
    // exercises the actual backend snapshot writer and its caller state.
    {
        // The reviewer's exact scenario: 20 m stored at -12, connect pins a
        // gain, then a same-band tune triggers a capture. The capture must
        // record -12. The pin was written as 20 when the ceiling was +48; at
        // +19 that value no longer survives the clamp, so the scenario is
        // carried by +18 -- the pin has to be a gain the radio can actually
        // apply, or the case is testing the clamp instead of the pin.
        const auto seed = connectLna(/*haveRestoredState=*/true,
                                     /*hasStoredEntry=*/true, /*storedDb=*/-12,
                                     /*paramPresent=*/true, /*paramDb=*/18,
                                     kDefault, kMin, kMax);
        check(seed.liveDb == 18 && seed.sessionPin,
              "snapshot: the pin is live at 18 and marked");
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

    // ── the connect param is CLAMPED to the range, and this is what proves it ──
    //
    // With the stale kMax = 48 this case read paramDb = 20 -> liveDb == 20 and
    // passed identically with and without the clamp. Against the real ceiling
    // it discriminates: delete clampDb from connectLna's param branch and this
    // goes red.
    {
        const ConnectLna out = connectLna(/*haveRestoredState=*/false,
                                          /*hasStoredEntry=*/false, /*storedDb=*/0,
                                          /*paramPresent=*/true, /*paramDb=*/20,
                                          /*defaultDb=*/kDefault, kMin, kMax);
        check(out.liveDb == kLnaGainMaxDb,
              "param: a connect asking for +20 comes up at the +19 ceiling");
    }
    {
        const ConnectLna out = connectLna(/*haveRestoredState=*/false,
                                          /*hasStoredEntry=*/false, /*storedDb=*/0,
                                          /*paramPresent=*/true, /*paramDb=*/-40,
                                          /*defaultDb=*/kDefault, kMin, kMax);
        check(out.liveDb == kLnaGainMinDb,
              "param: a connect asking below the floor comes up at the floor");
    }
    {
        // The pin comparison uses the CLAMPED value, so a param of 20 against a
        // stored 19 is not a pin -- they are the same setting once the hardware
        // has had its say.
        const ConnectLna out = connectLna(/*haveRestoredState=*/true,
                                          /*hasStoredEntry=*/true, /*storedDb=*/19,
                                          /*paramPresent=*/true, /*paramDb=*/20,
                                          /*defaultDb=*/kDefault, kMin, kMax);
        check(out.liveDb == 19 && !out.sessionPin,
              "param: +20 against a stored +19 is not a session pin");
    }

    // ── a stored gain from before the ceiling moved is MIGRATED, not clamped ──
    //
    // Every value here is what the radio was ACTUALLY applying for that stored
    // number, through the old ccRxGain (code clamped at 60) and the gateware's
    // five-bit decode. Clamping instead would hand a stored 20 a +31 dB jump on
    // the first connect after an update.
    {
        check(migrateStoredLnaDb(20) == -12,
              "migrate: a stored +20 was being applied as -12 dB, and still is");
        check(migrateStoredLnaDb(24) == -8,  "migrate: stored +24 -> -8 dB");
        check(migrateStoredLnaDb(32) == 0,   "migrate: stored +32 -> 0 dB");
        check(migrateStoredLnaDb(48) == 16,  "migrate: stored +48 -> +16 dB");
        // In range: untouched, including both ends.
        check(migrateStoredLnaDb(19) == 19,  "migrate: +19 is in range and unchanged");
        check(migrateStoredLnaDb(-12) == -12, "migrate: the floor is unchanged");
        check(migrateStoredLnaDb(0) == 0,    "migrate: the new default is unchanged");
        // Below the floor is unreachable through the old encode -- codes clamp
        // at 0 -- so there is nothing to reconstruct.
        check(migrateStoredLnaDb(-40) == -12,
              "migrate: below the floor clamps, because no fold produced it");
        // WHATEVER it is handed, the result is a value this radio can apply.
        for (int stored = -200; stored <= 200; ++stored) {
            const int out = migrateStoredLnaDb(stored);
            if (out < kLnaGainMinDb || out > kLnaGainMaxDb) {
                check(false, "migrate: every result is inside the AD9866 range");
                break;
            }
        }
        check(true, "migrate: every result over -200..200 is inside the range");
    }

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
