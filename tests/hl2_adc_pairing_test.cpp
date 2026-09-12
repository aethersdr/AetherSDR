// The HL2's pre-DDC / post-DDC ADC pairing, as a deterministic decision.
//
// HERMES.md §13 item 16. Most of the behaviour this pins is otherwise reachable
// only by driving a real AD9866 into a real overload with a real strong signal
// placed inside or outside the slice — which is not something a test suite can
// arrange, and is exactly why the decision is a seam rather than an inline
// condition in healthSnapshot(). Hl2Backend evaluates these same functions, so
// what passes here is what the radio runs
// (core/backends/hl2/Hl2AdcPairing.h).
//
// TWO cases are different — 7 and 8, the two liveness gates. Each is a way the
// slice side stops being a reading of now while the pre-DDC flag keeps moving,
// each needs no saturated converter to construct, and each asserted the
// opposite of what was happening before its input existed: case 7 for the tail
// of a transmission, case 8 for its head. Case 8 is here because case 7 alone
// let the head through — the first kSliceStaleMs of a key-down arrives with
// sliceReadingIsCurrent still TRUE, and until the mute became an input to this
// function no argument to it could say so. That is the seam paying for itself,
// twice, and the second time because the first cover was incomplete.
//
// What is NOT asserted here: any absolute level. Neither side of the pairing
// is calibrated and they do not share a scale, so there is no dBFS figure this
// file could check against anything. What it checks is the RELATIONSHIP, which
// is the only thing the pairing claims.

#include "core/backends/hl2/Hl2AdcPairing.h"

#include <cstdio>
#include <limits>

using AetherSDR::hl2::adcMeterReadingIsReal;
using AetherSDR::hl2::AdcPairing;
using AetherSDR::hl2::adcPairing;
using AetherSDR::hl2::kSliceHotHeadroomDb;
using AetherSDR::hl2::kSliceStaleMs;
using AetherSDR::hl2::sliceHeadroomDb;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

// Shorthands for the two sides, so each case below reads as the physical
// situation rather than as six positional booleans. `paired` is the receiving
// radio: both sides reporting, the slice side still being sampled, and its last
// reading still recent.
AdcPairing paired(double slicePeakDbfs, bool hardwareOverload)
{
    return adcPairing(/*haveSlicePeak=*/true, slicePeakDbfs,
                      /*sliceReadingIsCurrent=*/true,
                      /*sliceSideSampling=*/true,
                      /*haveHardwareFlag=*/true, hardwareOverload);
}

// THE TAIL OF A TRANSMISSION, or any other stall: the slice side stopped long
// enough ago that its age has grown past kSliceStaleMs.
AdcPairing pairedStale(double slicePeakDbfs, bool hardwareOverload)
{
    return adcPairing(/*haveSlicePeak=*/true, slicePeakDbfs,
                      /*sliceReadingIsCurrent=*/false,
                      /*sliceSideSampling=*/true,
                      /*haveHardwareFlag=*/true, hardwareOverload);
}

// THE HEAD OF A TRANSMISSION — the case pairedStale() cannot express and the
// one that got through the first fix. Sampling has just stopped, so the held
// reading is still CURRENT by every age test: `sliceReadingIsCurrent` is true
// here on purpose, exactly as Hl2Backend computes it in the first ~140 ms after
// key-down. Only the synchronous mute tells the truth this early.
AdcPairing pairedKeyDown(double slicePeakDbfs, bool hardwareOverload)
{
    return adcPairing(/*haveSlicePeak=*/true, slicePeakDbfs,
                      /*sliceReadingIsCurrent=*/true,
                      /*sliceSideSampling=*/false,
                      /*haveHardwareFlag=*/true, hardwareOverload);
}

}  // namespace

int main()
{
    // ---- 1. THE DIAGNOSTIC CASE -------------------------------------------
    //
    // The converter is overloading and this slice is 40 dB down. A3 §7 calls
    // the pairing the most useful diagnostic on this radio because of exactly
    // this: the clip flag alone says "reduce gain", the slice peak alone says
    // "there is nothing here", and only together do they say "the signal
    // saturating the ADC is not the one you are listening to".
    check(paired(-40.0, /*overload=*/true) == AdcPairing::ConverterOnly,
          "quiet slice + converter overload is ConverterOnly — the disagreement");
    check(paired(-6.0, true) == AdcPairing::ConverterOnly,
          "still ConverterOnly with real headroom in the slice");

    // ---- 2. The case where they AGREE -------------------------------------
    //
    // Both hot: the strong signal really is in this slice, and either number
    // alone would have been enough. Distinguishing this from case 1 is the
    // entire value of the pairing, so it must not collapse into it.
    check(paired(-1.0, true) == AdcPairing::BothHot,
          "hot slice + converter overload is BothHot — the signal is in the slice");
    check(paired(-40.0, false) == AdcPairing::BothClear,
          "quiet slice + no overload is BothClear");

    // ---- 3. The OTHER disagreement ----------------------------------------
    //
    // A slice near wire full scale with the converter perfectly happy. Real,
    // and it points downstream rather than at the front end: the level is
    // arriving through the DDC, not through the antenna port. It must not be
    // reported as a front-end overload, because attenuating the front end
    // would be the wrong response to it.
    check(paired(-0.5, false) == AdcPairing::SliceOnly,
          "hot slice + no overload is SliceOnly — not a front-end problem");

    // ---- 4. The hot boundary is INCLUSIVE, and it is a display boundary ----
    //
    // Exactly kSliceHotHeadroomDb of headroom counts as hot. The value is a
    // display boundary rather than a calibrated one — there is no calibrated
    // boundary to have — so what matters is that it is applied consistently
    // and does not jitter between verdicts at its own value.
    check(paired(-kSliceHotHeadroomDb, false) == AdcPairing::SliceOnly,
          "exactly at the hot boundary counts as hot (no overload)");
    check(paired(-kSliceHotHeadroomDb, true) == AdcPairing::BothHot,
          "exactly at the hot boundary counts as hot (overload)");
    check(paired(-kSliceHotHeadroomDb - 0.01, false) == AdcPairing::BothClear,
          "a hair below the boundary is clear");

    // ---- 5. "NEVER HEARD" IS NOT A LEVEL ----------------------------------
    //
    // Both sides can be absent, and absent is not zero. The HL2 reports its
    // overload bit only once EP6 RADDR 0x00 has arrived, and the WDSP meter
    // has no value until a block has been processed. Either missing makes the
    // pairing unanswerable — and an unanswerable pairing must say so, not
    // report the reading it does have as though it were the whole story.
    check(adcPairing(/*haveSlicePeak=*/false, 0.0, /*current=*/true, /*sampling=*/true,
                     true, true)
              == AdcPairing::Unknown,
          "no slice reading yet is Unknown, not a level");
    check(adcPairing(true, -40.0, /*current=*/true, /*sampling=*/true,
                     /*haveHardwareFlag=*/false, false)
              == AdcPairing::Unknown,
          "overload bit never seen is Unknown, not 'clear'");
    check(adcPairing(false, 0.0, true, true, false, false) == AdcPairing::Unknown,
          "neither side reported is Unknown");

    // ---- 6. WDSP's OWN SENTINELS are not measurements ----------------------
    //
    // meter.c writes -400.0 for a stage that is not running and its
    // 10*log10(peak + 1e-40) floors there for an identically-zero input;
    // WdspChannel::meter() returns -300.0 for a transmit channel, which has no
    // RXA meters at all. A real HF slice always carries noise, so none of these
    // is a level an antenna can produce. Reporting one as "-400 dBFS" would
    // dress a missing reading up as a measurement of a dead band.
    check(!adcMeterReadingIsReal(-400.0), "WDSP's not-running floor is not a reading");
    check(!adcMeterReadingIsReal(-300.0), "the transmit-channel sentinel is not a reading");
    check(!adcMeterReadingIsReal(std::numeric_limits<double>::quiet_NaN()),
          "NaN is not a reading");
    check(adcMeterReadingIsReal(-120.0), "a genuinely quiet slice IS a reading");
    check(adcMeterReadingIsReal(0.0), "full scale IS a reading");
    check(paired(-400.0, true) == AdcPairing::Unknown,
          "a sentinel slice reading cannot make a pairing, even with the flag set");

    // ---- 7. A STALE SLICE READING CANNOT BE PAIRED — THE TAIL -------------
    //
    // The two sides stop at different times. Hl2RxDsp holds the slice peak at
    // its last receive value for the whole of a transmission — it must, the
    // chain is clocked with silence there — while the pre-DDC flag keeps
    // updating, because EP6 responses ride the same datagrams as the IQ and RX
    // streaming continues through TX on the HL2.
    //
    // On an HL2 the transmitter is on the SAME PORT as the receiver, so a
    // converter overload while keyed is a routine reading and the signal doing
    // it is the operator's own carrier, squarely inside the slice. Paired
    // against a frozen quiet slice it would come out as "the signal doing it is
    // elsewhere in 0-38.4 MHz" — the exact opposite, and it would send the
    // operator to the attenuator to fix their own PTT.
    //
    // This covers a transmission from kSliceStaleMs onward, and every other way
    // the DSP thread can stop producing while EP6 keeps arriving. It does NOT
    // cover the first kSliceStaleMs — that is case 8, and it is the gap this
    // case was wrongly believed to close.
    check(pairedStale(-40.0, /*overload=*/true) == AdcPairing::Unknown,
          "frozen quiet slice + live overload is Unknown, NOT 'the signal is elsewhere'");
    check(pairedStale(-1.0, true) == AdcPairing::Unknown,
          "a stale slice cannot claim BothHot either — it is not a reading of now");
    check(pairedStale(-40.0, false) == AdcPairing::Unknown,
          "staleness is not 'nothing is happening': BothClear is a claim too");
    check(pairedStale(-0.5, false) == AdcPairing::Unknown,
          "and it is not SliceOnly");
    // The same value, freshly observed, is a verdict again — so the gate is on
    // the age and not on the level.
    check(paired(-40.0, true) == AdcPairing::ConverterOnly,
          "the identical reading, current, still pairs");

    // ---- 8. THE HEAD OF A TRANSMISSION, WHERE THE AGE IS STILL HONEST -----
    //
    // The case that case 7 could not express, and the reason the first fix
    // shipped with the fault still in it. At key-down the last receive block is
    // at most
    // one block period old — 21.3 ms at 48 kHz — so `ago <= kSliceStaleMs` is
    // TRUE and stays true while the age climbs, for 129-150 ms depending on
    // where key-down fell inside a block. For that whole window the pre-transmit
    // peak, which is genuinely fresh, is paired against a live flag that is now
    // reading the operator's own carrier. K5PTB measured ConverterOnly to
    // t=137 ms against a real Hl2RxDsp, and RadioHealthDialog refreshes every
    // 500 ms, so it is on screen for roughly three key-downs in ten.
    //
    // So the freshness of the reading is NOT the whole question. Whether the
    // side is still being sampled is the other half, the backend knows it
    // synchronously because it queues the mute itself, and these assertions are
    // what makes that half of the gate a property of this function rather than
    // of one call site nothing tests.
    check(pairedKeyDown(-40.0, /*overload=*/true) == AdcPairing::Unknown,
          "key-down: a FRESH quiet slice + live overload is Unknown, not 'elsewhere'");
    check(pairedKeyDown(-1.0, true) == AdcPairing::Unknown,
          "key-down: not BothHot either — the slice reading predates the carrier");
    check(pairedKeyDown(-40.0, false) == AdcPairing::Unknown,
          "key-down: BothClear is a claim about now, and this is not now");
    check(pairedKeyDown(-0.5, false) == AdcPairing::Unknown,
          "key-down: and it is not SliceOnly");
    // The distinguishing assertion, and the one that fails if the new input is
    // folded into the age: the reading here is CURRENT by every age test. Only
    // the sampling side of the gate can refuse it.
    check(paired(-40.0, true) == AdcPairing::ConverterOnly
              && pairedKeyDown(-40.0, true) == AdcPairing::Unknown,
          "identical reading, identically current — only 'still sampling' differs");
    // TX MONITOR. setKeying mutes on `key && !m_txMonitor`, so with the monitor
    // on the chain keeps being clocked with real IQ, the meter keeps moving,
    // and the pairing must keep pairing THROUGH a transmission. A gate written
    // against `m_keyed` alone would blank this, and the monitor is the one mode
    // where the row has something to say about our own carrier.
    check(adcPairing(/*haveSlicePeak=*/true, -1.0, /*current=*/true,
                     /*sliceSideSampling=*/true, /*haveHardwareFlag=*/true,
                     /*hardwareOverload=*/true)
              == AdcPairing::BothHot,
          "keyed with the TX monitor on still samples, so it still pairs");

    // ---- 9. The staleness threshold is CHOSEN, and it is a duration --------
    //
    // Not a measurement and not a round number: it sits in the gap between one
    // output block (21.3 ms at the HL2's slowest rate, so a threshold near
    // there would call a healthy receive path stale on scheduling jitter) and
    // the shortest transmission an operator can deliberately make (a PTT tap,
    // a couple of hundred milliseconds; CW break-in holds MOX through its
    // inter-element hang, so even one character is one keyed span). Pinning
    // the bracket rather than the value is the honest assertion — the value
    // may move inside it, and the verdict must not become a sentence about a
    // frozen reading if it does.
    check(kSliceStaleMs > 21, "longer than one output block, or receive goes stale");
    check(kSliceStaleMs < 200, "shorter than a PTT tap, or a transmission still lies");

    // ---- 10. Headroom is the number the readout states ---------------------
    //
    // Positive below full scale, and it is measured against WIRE full scale —
    // the sign convention is the one the sentence in healthSnapshot() reads
    // out, so getting it backwards would invert every verdict's explanation.
    check(sliceHeadroomDb(-40.0) == 40.0, "40 dB down is 40 dB of headroom");
    check(sliceHeadroomDb(0.0) == 0.0, "at full scale there is no headroom");

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
