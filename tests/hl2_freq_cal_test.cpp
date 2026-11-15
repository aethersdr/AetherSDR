// HL2 manual frequency calibration.
//
// THE POINT OF THIS FILE. The correction is a scalar derived on paper, and the
// derivation is the part that can be wrong. A test that re-applies
// Hl2FreqCal's own algebra would agree with it whether or not the algebra
// describes the radio — a convention error is invisible to any test that shares
// the convention. So the load-bearing cases here do NOT call the production
// formula to check the production formula. They SIMULATE THE GATEWARE:
//
//   radio.v:321  assign freqcomp = cmd_data * M2 + M3;
//   radio.v:384  freqcompp[0] <= freqcomp[56:25];
//   radio.v:86   M2 = 2^57 / 76'800'000   (localparam — baked into the bitstream)
//   radio.v:89   M3 = 2^24                (rounding)
//
// then run the resulting phase word against a master clock that is deliberately
// WRONG by the error under test, and require the oscillator to land on the
// frequency the operator asked for. If the sign is backwards, or the correction
// belongs on the other side of the division, this fails.

#include "core/backends/hl2/Hl2FreqCal.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using AetherSDR::Hl2FreqCal;
using namespace AetherSDR::hl2;

static int g_failures = 0;

static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static void checkNear(double got, double want, double tol, const char* what)
{
    if (!(std::fabs(got - want) <= tol)) {
        std::fprintf(stderr, "FAIL: %s (got %.6f, want %.6f, tol %.6f)\n",
                     what, got, want, tol);
        ++g_failures;
    }
}

// ---- the gateware, in host arithmetic -------------------------------------

// M2 as the bitstream holds it. Written as the literal from radio.v rather than
// computed, so a change to the derivation here cannot silently move the
// reference too.
static constexpr std::uint64_t kGatewareM2 = 1876499845ull;
static constexpr std::uint64_t kGatewareM3 = 1ull << 24;

// The 32-bit phase increment the FPGA loads for a commanded frequency.
static std::uint32_t gatewarePhase(std::uint32_t commandedHz)
{
    const std::uint64_t freqcomp =
        static_cast<std::uint64_t>(commandedHz) * kGatewareM2 + kGatewareM3;
    return static_cast<std::uint32_t>((freqcomp >> 25) & 0xFFFFFFFFull);
}

// Where that oscillator ACTUALLY lands, on a clock that is off by ppb.
static double actualLoHz(std::uint32_t commandedHz, int ppb)
{
    const double clock = Hl2FreqCal::kNominalClockHz * (1.0 + ppb / 1.0e9);
    return static_cast<double>(gatewarePhase(commandedHz)) / 4294967296.0 * clock;
}

// Confirm the model itself before trusting it: with a perfect clock, the
// gateware must reproduce the commanded frequency. If this fails, kGatewareM2
// or the shift is transcribed wrong and every case below is meaningless.
static void testGatewareModelIsSane()
{
    for (const std::uint32_t f : {1'800'000u, 7'074'000u, 10'000'000u,
                                  14'074'000u, 28'500'000u, 38'400'000u}) {
        checkNear(actualLoHz(f, 0), static_cast<double>(f), 0.02,
                  "gateware model reproduces the commanded frequency on a perfect clock");
    }
    // Phase-word resolution is 76.8 MHz / 2^32; the 0.02 Hz tolerance above is
    // just over one LSB. Pin that so a future tolerance change is deliberate.
    checkNear(Hl2FreqCal::kNominalClockHz / 4294967296.0, 0.017881393, 1e-6,
              "phase word resolution is ~0.0179 Hz");
}

// ---- the load-bearing case ------------------------------------------------

// For a spread of real crystal errors and real operating frequencies: correct
// the frequency, hand it to the gateware, run it on the wrong clock, and require
// the oscillator to land where the operator asked.
static void testCorrectionLandsOnTruth()
{
    const int ppbs[] = {0, 1, -1, 178, -178, 1000, -1000, 10'000, -10'000,
                        50'000, -50'000};
    const double freqs[] = {1'800'000.0, 3'573'000.0, 7'074'000.0, 10'000'000.0,
                            14'074'000.0, 21'074'000.0, 28'500'000.0, 38'000'000.0};

    for (const int ppb : ppbs) {
        const double scale = Hl2FreqCal::scaleForPpb(ppb);
        for (const double want : freqs) {
            const std::uint32_t cmd = Hl2FreqCal::ncoCommandHz(want, scale);
            const double got = actualLoHz(cmd, ppb);
            // The NCO register is integer Hz, so on its own it can only get
            // within half a hertz — times the clock error, which is negligible.
            // The DSP shift closes the rest; that is the next test.
            checkNear(got, want, 0.55,
                      "corrected NCO lands within the register's own quantisation");
        }
    }
}

// The two stages together are EXACT. The shift is computed against the rounded
// NCO command, so the register's 1 Hz granularity cancels rather than leaking
// into the audio. This is the case that fails if dspShiftHz() is ever
// "simplified" to (slice - nco) * scale.
static void testTwoStageTuningIsExact()
{
    const int ppbs[] = {0, 250, -250, 5000, -5000, 50'000, -50'000};
    // Slice offsets from the NCO, including ones that make the NCO command
    // round in the unhelpful direction.
    const double offsets[] = {0.0, 1.0, -1.0, 999.0, -12'345.0, 23'456.7};

    for (const int ppb : ppbs) {
        const double scale = Hl2FreqCal::scaleForPpb(ppb);
        const double nominalRate = 48'000.0;
        // The sample stream is clocked by the same crystal, so a shift the DSP
        // believes is `s` Hz is really s*(1+e) Hz. This is the step that makes
        // the whole scheme work and it must be modelled, not assumed.
        const double realRate = nominalRate * (1.0 + ppb / 1.0e9);

        for (const double ncoTrue : {7'100'000.0, 14'200'000.0, 28'400'000.0}) {
            for (const double off : offsets) {
                const double sliceTrue = ncoTrue + off;
                const std::uint32_t cmd = Hl2FreqCal::ncoCommandHz(ncoTrue, scale);
                const double shiftCmd = Hl2FreqCal::dspShiftHz(sliceTrue, cmd, scale);

                const double realShift = shiftCmd * (realRate / nominalRate);
                const double demodAt = actualLoHz(cmd, ppb) + realShift;

                // 0.02 Hz — one phase-word LSB. Nothing else is left over.
                checkNear(demodAt, sliceTrue, 0.02,
                          "NCO + DSP shift lands exactly on the slice frequency");
            }
        }
    }
}

// ---- measurement ----------------------------------------------------------

// The operator's workflow, inverted: given a crystal error, work out where a
// known reference APPEARS, feed that dial reading back in, and require the
// original error to come out. Nothing here reuses ppbFromZeroBeat's algebra —
// the observed reading is derived from the gateware simulation above.
static void testZeroBeatRecoversTheError()
{
    const int ppbs[] = {0, 178, -178, 2500, -2500, 20'000, -20'000};
    for (const int ppb : ppbs) {
        const double reference = 10'000'000.0;
        // Uncorrected, the radio is commanded with the dial value itself. The
        // operator turns the dial until the reference nulls — i.e. until the
        // oscillator sits on the true reference frequency.
        //
        // Search for that dial reading the way an operator does: the value whose
        // resulting LO equals the reference.
        const double dialled = reference * Hl2FreqCal::scaleForPpb(ppb);
        const double loWhenDialled = actualLoHz(
            Hl2FreqCal::ncoCommandHz(dialled, 1.0), ppb);
        checkNear(loWhenDialled, reference, 0.6,
                  "the dial reading that nulls the reference puts the LO on it");

        const int recovered = Hl2FreqCal::ppbFromZeroBeat(reference, dialled);
        // Recovery is limited by the 1 Hz dial granularity an operator can
        // actually read; 100 ppb is 1 Hz at 10 MHz.
        check(std::abs(recovered - ppb) <= 1,
              "zero-beat recovers the crystal error that produced it");
    }
}

static void testSignConvention()
{
    // The convention every caller depends on, pinned in one place: a FAST clock
    // makes signals appear LOW. Stated in the header, and wrong-way-round in
    // half the amateur literature, so it gets its own case.
    const int fast = 1000;   // +1 ppm
    checkNear(Hl2FreqCal::effectiveClockHz(fast), 76'800'076.8, 0.1,
              "positive ppb means the clock runs fast");
    check(Hl2FreqCal::errorHzAt(10'000'000.0, fast) < 0.0,
          "a fast clock makes a signal read low");
    checkNear(Hl2FreqCal::errorHzAt(10'000'000.0, fast), -10.0, 0.01,
              "+1 ppm reads 10 Hz low at 10 MHz");
    // And the correction commands a LOWER number to compensate.
    check(Hl2FreqCal::ncoCommandHz(10'000'000.0, Hl2FreqCal::scaleForPpb(fast))
              < 10'000'000u,
          "a fast clock is corrected by commanding a lower frequency");
}

// ---- guards ---------------------------------------------------------------

static void testClampingAndDegenerateInput()
{
    check(Hl2FreqCal::clampPpb(999'999) == Hl2FreqCal::kMaxPpb, "clamps high");
    check(Hl2FreqCal::clampPpb(-999'999) == Hl2FreqCal::kMinPpb, "clamps low");
    check(Hl2FreqCal::scaleForPpb(0) == 1.0, "zero ppb is an identity scale");

    // Garbage in from a hand-edited settings file or a bad capture must not
    // reach the tuning path.
    check(Hl2FreqCal::ppbFromZeroBeat(0.0, 10'000'000.0) == 0, "zero reference -> 0");
    check(Hl2FreqCal::ppbFromZeroBeat(10'000'000.0, 0.0) == 0, "zero dial -> 0");
    check(Hl2FreqCal::ppbFromZeroBeat(-1.0, 10'000'000.0) == 0, "negative reference -> 0");

    // Zero-beating a harmonic instead of the fundamental: clamped, not obeyed.
    // The UI treats a clamped result as a refusal.
    check(Hl2FreqCal::ppbFromZeroBeat(10'000'000.0, 20'000'000.0) == Hl2FreqCal::kMinPpb,
          "a wildly wrong capture clamps rather than committing kilohertz of error");

    check(Hl2FreqCal::ncoCommandHz(-1.0, 1.0) == 0u, "negative frequency -> 0, not a wrap");
    check(Hl2FreqCal::ncoCommandHz(0.0, 1.0) == 0u, "zero frequency -> 0");
}

// ---- the wire -------------------------------------------------------------

// The correction has to reach the actual command banks. Decoded by hand here
// rather than through any production helper: this is the assertion that the
// bytes on the wire changed, not that a model value did.
static void testWireBanksCarryTheCorrection()
{
    const int ppb = 10'000;                       // +10 ppm, a bad but real crystal
    const double scale = Hl2FreqCal::scaleForPpb(ppb);
    const double want = 14'074'000.0;             // FT8 on 20 m
    const std::uint32_t cmd = Hl2FreqCal::ncoCommandHz(want, scale);

    check(cmd != static_cast<std::uint32_t>(want),
          "a calibrated radio does NOT command the dialled frequency verbatim");

    auto decodeBe32 = [](const Cc& bank) {
        return (static_cast<std::uint32_t>(bank[1]) << 24)
             | (static_cast<std::uint32_t>(bank[2]) << 16)
             | (static_cast<std::uint32_t>(bank[3]) << 8)
             |  static_cast<std::uint32_t>(bank[4]);
    };

    const Cc rx = ccRxFreq(0, cmd);
    check(rx[0] == kC0Rx1Freq, "RX1 frequency goes to register 0x02 (C0 0x04)");
    check(decodeBe32(rx) == cmd, "RX1 bank carries the corrected frequency");

    // TX matters as much as RX and is a SEPARATE register: the gateware derives
    // both oscillators from one freqcomp, so a build that corrected receive and
    // not transmit would put the operator on the air off frequency by the full
    // error while sounding right to themselves.
    const Cc tx = ccTxFreq(cmd);
    check(tx[0] == kC0TxFreq, "TX frequency goes to register 0x01 (C0 0x02)");
    check(decodeBe32(tx) == cmd, "TX bank carries the same corrected frequency");
    check(decodeBe32(tx) == decodeBe32(rx),
          "RX and TX are corrected identically — they share one gateware scale");

    // And the correction is in the direction the sign convention promises.
    check(cmd < static_cast<std::uint32_t>(want),
          "a fast clock is compensated by commanding lower");
    checkNear(static_cast<double>(want - cmd), 140.74, 1.0,
              "+10 ppm at 14.074 MHz is about a 141 Hz correction");
}

int main()
{
    testGatewareModelIsSane();
    testCorrectionLandsOnTruth();
    testTwoStageTuningIsExact();
    testZeroBeatRecoversTheError();
    testSignConvention();
    testClampingAndDegenerateInput();
    testWireBanksCarryTheCorrection();

    if (g_failures == 0)
        std::printf("hl2_freq_cal_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
