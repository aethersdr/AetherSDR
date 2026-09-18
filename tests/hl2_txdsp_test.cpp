// SSB modulator correctness for the Hermes-Lite 2 transmit chain.
//
// The assertion that matters on the air: for USB, a 1 kHz audio tone must leave
// this modulator at -1 kHz of the IQ it hands to the wire, and for LSB at
// +1 kHz.
//
// THAT SIGN LOOKS BACKWARDS AND IS NOT. The HPSDR wire order has the opposite
// handedness to the standard analytic convention, which is why the receive path
// conjugates with -imag() before WDSP sees anything. Transmit carries the same
// correction, so the wire-facing sign is inverted from the textbook one.
//
// This was originally asserted the textbook way, and the test passed while every
// transmission went out on the WRONG SIDEBAND — invisible from inside the
// application, because the panadapter reads the same wire order and therefore
// agreed with it. It took an operator with a second receiver to catch it.
//
// It also pins the rate conversion (24 kHz audio in, 48 kHz IQ out) and the
// mic-gain path, both of which are easy to get subtly wrong in ways that only
// show up as "my audio is quiet" or "my signal is 2 kHz wide".

#include "core/backends/hl2/Hl2TxDsp.h"
#include "TxTestAuthority.h"
#include "core/backends/hl2/Hl2TxLevelPolicy.h"

#include <QCoreApplication>
#include <QObject>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <thread>
#include <vector>

using namespace AetherSDR::hl2;
using AetherSDR::TxAudioSource;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

// ── The caller's cadence, which the TXA build depends on and the phasing ──
//    build does not.
//
// Hl2TxDsp's TXA channel is opened with blockForOutput = false, so
// WdspChannel::processIq RETURNS Underrun rather than waiting when the output
// side is not ready. A test that pushes a second of audio through in a tight
// loop is not driving the modulator, it is STARVING it: run unpaced, this file
// emitted 1024 IQ samples where it expected 23552, on exact zeros, with every
// sideband assertion reading 0.0 dB. The live caller does not do that --
// AudioEngine hands this stage audio as the sound card produces it.
//
// Pace TXA at the configured audio rate. Feeding five times faster can exhaust
// scheduling headroom under sanitizers and invalidate otherwise correct captures.
// The synchronous phasing implementation does not need a wall-clock delay.
// Carry the engine-issued context through every feed: a missing grant is dropped.
static void feed(Hl2TxDsp& tx, const std::vector<float>& chunk,
                 TxAudioSource source, const AetherSDR::TxCoordinator::Context& context)
{
    tx.processAudioBlock(chunk, source, context);
    if (AETHER_HL2_TX_TXA) {
        std::this_thread::sleep_for(std::chrono::duration<double>(
            static_cast<double>(chunk.size()) / tx.config().inputSampleRateHz));
    }
}

// Trim an IQ capture to a WHOLE number of cycles of `hz`, after dropping `skip`
// samples of filter settling.
//
// THIS IS NOT COSMETIC, and the sweep below is unmeasurable without it.
//
// binPower correlates against a rectangular window. The leakage of a strong
// tone at +f into the bin at -f therefore falls off only as the Dirichlet
// kernel, roughly 1/(pi * 2f * N/fs) -- for the capture lengths this file uses
// that is a floor somewhere around 60-75 dB, and at a 150 Hz tone (300 Hz of
// bin separation) it is about 60 dB. The modulator's real suppression at
// mid-band is ~87 dB, which is BELOW that floor: an untrimmed sweep would
// measure its own analysis window and report it as the modulator's figure.
//
// Truncating to a whole number of cycles puts the image bin exactly on a null
// of the kernel and the leakage term vanishes, because the bin spacing fs/N
// then divides the 2f separation exactly. The period is fs/gcd(f, fs) samples,
// which is why every tone in the sweep is an integer number of hertz.
static std::vector<std::complex<float>> wholeCycles(
    const std::vector<std::complex<float>>& iq, double hz, double fs,
    std::size_t skip)
{
    if (iq.size() <= skip)
        return {};
    const long f = std::lround(hz);
    const long r = std::lround(fs);
    if (f <= 0 || r <= 0)
        return {};
    const std::size_t period = static_cast<std::size_t>(r / std::gcd(f, r));
    const std::size_t avail = iq.size() - skip;
    const std::size_t n = (avail / period) * period;
    if (n == 0)
        return {};
    return {iq.begin() + static_cast<std::ptrdiff_t>(skip),
            iq.begin() + static_cast<std::ptrdiff_t>(skip + n)};
}

// Output samples dropped from the FRONT of every capture before it is
// analysed.
//
// Both modulators have a priming stretch and neither one's is a filter
// response. The phasing modulator's is its 255-tap delay line filling, about
// 5 ms. A TXA channel's is create_slews' mute ramp (ndelup + ntup = 840 input
// samples = 35 ms) plus the rest of the fill, measured as bounded at output
// sample 2336 -- 48.7 ms -- after which the output never returns to zero. A
// ramp is amplitude modulation, and amplitude modulation puts energy in the
// image bin, so including it would report the envelope rather than the filter.
//
// 12288 samples is 12 output blocks, 256 ms. 4096 was tried first and is NOT
// enough, which the instrument said rather than the arithmetic: the census's
// per-block RMS walks 0, 0, 7.8e-14, 2.2e-04, 4.3e-02 and only reaches its
// settled 7.07e-02 at block 5, so a 4096-sample skip lands inside the ramp and
// the residual amplitude modulation held the image bin at 4.4e-05 -- an 81 dB
// figure that is the ENVELOPE, not the filter. 12288 clears block 5 by a
// factor of 2.4.
//
// Applied in BOTH builds, because the point of this instrument is that the two
// chains are read by one of it.
static constexpr std::size_t kSettleSamples = 12288;

// Longest prefix that is a WHOLE number of cycles of `hz`.
//
// THIS IS NOT TIDINESS, and without it the assertions below measure their own
// analysis window rather than the modulator.
//
// binPower correlates against a RECTANGULAR window, so a strong component at
// -f leaks into the bin at +f through the Dirichlet kernel -- about
// wanted * fs / (pi * 2f * N) for a capture of N samples. This file's captures
// are NOT whole-cycle by accident: 1.0 s of 24 kHz audio is 46 modulator
// blocks, 23552 samples consumed, 47104 out, and 47104 is not a whole number
// of 1 kHz periods. The leakage floor there is about 76 dB, which is why this
// file could only ever assert 30 dB and why the 87 dB the phasing modulator
// actually reaches at 1 kHz was invisible to it.
//
// Truncating to a whole number of cycles puts the image bin exactly on a null
// of the kernel. The period is fs/gcd(f, fs) samples, which is why every tone
// asserted here is an integer number of hertz. The capture's start offset does
// not matter: an offset only rotates the leakage term's phase, it does not
// stop it summing to zero -- which is what licenses kSettleSamples above.
//
// This is the same correction wdsp_channel_test's wholeCycleCount() makes.
// Both chains have to be measured through the same instrument or the
// comparison between them is not one.
static std::size_t wholeCycles(std::size_t have, double hz, double fs)
{
    const long f = std::lround(hz);
    const long r = std::lround(fs);
    if (f <= 0 || r <= 0) {
        return 0;
    }
    const std::size_t period = static_cast<std::size_t>(r / std::gcd(f, r));
    return (have / period) * period;
}

// Goertzel-style complex bin: correlate the IQ against exp(-j*2*pi*f*n/fs).
// Positive f probes the upper sideband, negative the lower.
//
// `trimmed` drops the priming stretch and truncates to whole cycles. Left OFF
// for the diagnostic prints that only want a level, and ON everywhere a
// suppression figure is asserted.
static double binPower(const std::vector<std::complex<float>>& iq, double hz,
                       double fs, bool trimmed = false)
{
    std::size_t from = 0;
    std::size_t count = iq.size();
    if (trimmed) {
        if (iq.size() <= kSettleSamples) {
            return 0.0;
        }
        from = kSettleSamples;
        count = wholeCycles(iq.size() - kSettleSamples, hz < 0.0 ? -hz : hz, fs);
        if (count == 0) {
            return 0.0;
        }
    }
    std::complex<double> acc{0.0, 0.0};
    const double w = -2.0 * M_PI * hz / fs;
    for (std::size_t n = 0; n < count; ++n) {
        const double ph = w * static_cast<double>(n);
        acc += std::complex<double>(iq[from + n].real(), iq[from + n].imag())
             * std::complex<double>(std::cos(ph), std::sin(ph));
    }
    return std::abs(acc) / static_cast<double>(count);
}

// ── THE INSTRUMENT'S OWN NUMERICAL FLOOR ─────────────────────────────────
//
// THIS FILE HAS TWO FLOORS AND ONLY ONE OF THEM WAS EVER WRITTEN DOWN.
// wholeCycles() above documents the first: the analysis window's LEAKAGE
// floor, 60-75 dB at these capture lengths, and it disposes of that one by
// truncating to a whole number of cycles so the Dirichlet term nulls exactly.
// What is left standing after that null is the NUMERICAL floor, and in the TXA
// build it is what EVERY figure this file prints is actually reporting.
//
// WHERE IT COMES FROM. binPower accumulates in double, but the samples it
// accumulates are float. A capture at amplitude 0.5 carries a float32 rounding
// error near 0.5 * 2^-24 per sample, uncorrelated with the probe, so it sums
// as sqrt(N) and lands in the image bin around 0.5 * 2^-24 / sqrt(3N) -- about
// 9e-11 for the ~35000-sample captures here, a ratio near 195 dB for ONE tone,
// and lower than that for a capture carrying more than one. NOTHING IN THE
// MODULATOR HAS TO BE IMPERFECT FOR THAT NUMBER TO APPEAR. It is the
// representation, not the chain.
//
// MEASURED RATHER THAN ARGUED. A standalone replica of binPower() and
// wholeCycles(), fed signals whose image bin is ZERO BY CONSTRUCTION, reports:
//
//   capture (image is zero by construction)   150 Hz    1 kHz    2 kHz
//   a bare float exponential                   334.6    321.3    291.7
//   + a DC term at 0.01                        171.4    164.8    158.6
//   + four harmonics at -26 dB and that DC     171.3    166.9    186.9
//   + sixteen harmonics at -14 dB, DC 0.05     191.0    171.0    176.8
//
// So: 290-335 dB for a numerically clean exponential, and 158-191 dB for a
// capture carrying ANY realistic structure. The replica predicts 321.28 dB for
// the clean 1 kHz case and the USB headline below measures 320.71, which is
// what ties the replica to this test instead of leaving it an argument on
// paper.
//
// WHAT THAT MEANS FOR THE TXA BUILD. Every TXA figure in this file bar one
// group -- 167 to 332 dB -- sits at or above that floor, and NONE OF THOSE IS
// A MEASUREMENT OF THE MODULATOR. Each is UNRESOLVED: the true suppression may
// be higher or lower and this file cannot say which.
//
// AN EARLIER VERSION OF THIS COMMENT CALLED THEM LOWER BOUNDS. That was wrong,
// and the error is worth keeping because it is seductive. The reasoning was
// "the instrument's noise ADDS to the image bin, so the measured image can
// only be larger than the real one". That is scalar thinking about a COMPLEX
// quantity. The correlation error e is a complex vector added to the true
// image I, and |I + e| < |I| whenever e opposes I -- which needs only
// 0 < |e| < 2|I| and then happens for cos(theta) < -|e|/(2|I|).
// The reported ratio then EXCEEDS the true one.
//
// @jensenpat demonstrated it numerically on #5810 rather than arguing it: at
// 48 kHz, a 1 kHz tone over 34992 float samples with a tiny imaginary
// perturbation on sample zero, the double correlation reports 316.9406 dB
// while a 60-digit DFT of THE SAME float samples reports 316.8410 dB. The
// instrument overstated by 0.0996 dB. A bound that a counterexample exceeds is
// not a bound.
//
// So these figures are floor-limited ESTIMATES with no error bar. Giving them
// one would need a real uncertainty analysis of the correlation, which this
// file does not have and does not pretend to. (The exception is the bottom of a WIDE passband,
// where a TXA skirt is still resolvable and the sweep does measure it -- 59.37
// dB at 100 Hz on {0, 4000}. That paragraph is in the sweep's own comment.)
//
// The phasing build's figures (7.87 to 115.75 dB) are all well under the floor
// and are real measurements, which is why the characterisation sweep's
// baseline table belongs to the phasing build and is not a property of "the
// modulator".
//
// AND THE SWEEP BELOW CONFIRMS IT FROM INSIDE THIS TEST. In the TXA build its
// in-band rows do not form a curve, they alternate between the two regimes the
// replica predicts: USB reads 174.04 dB at 500 Hz, 173.67 at 700, then 322.33
// at 1000, 331.53 at 1500, 318.24 at 2000, then 171.98 again at 2500. A filter
// does not do that. What separates the ~170 dB rows from the ~320 dB ones is
// whether that tone's whole-cycle window happens to make the float samples a
// numerically clean exponential -- the top row of the table above -- or leaves
// structure in them. The raggedness IS the floor, showing its two faces.
//
// Set at the LOW end of the structured-capture range, so a figure is flagged
// as floor-limited whenever it might be rather than only when it certainly is.
static constexpr double kInstrumentFloorDb = 158.0;

// Mark a printed figure that is at or past that floor. A reader should not
// have to know this file's arithmetic to know which of its numbers are
// measurements and which are the arithmetic talking to itself.
static const char* floorMark(double suppDb)
{
    return suppDb >= kInstrumentFloorDb
        ? "  [>= instrument floor: UNRESOLVED, neither bound nor measurement]"
        : "";
}

// ── The divide guard for every opposite-sideband RATIO in this file ──────
//
// IT HAS TO SIT FAR BELOW THE INSTRUMENT'S FLOOR OR IT BECOMES THE
// MEASUREMENT, and at 1e-12 it did not. Three blocks -- the USB headline, the
// LSB headline and the mode-change case -- divided with 1e-12 while the TXA
// build's image bin lands near 1e-17, so all three printed
// 20*log10(0.5/1e-12) = 233.98 dB. A CONSTANT. They could not distinguish an
// image of exactly zero from an image of 5e-13, and what they reported was the
// value of their own guard. The DIGU low-edge block and the mode loop already
// used 1e-30 and were correct; this is that shape, applied everywhere.
//
// At 1e-30 the guard is thirteen decades under the smallest image bin this
// instrument can produce, so it perturbs no printed figure, and an image that
// really is zero prints 20*log10(0.5/1e-30) = 574 dB -- far above even the
// clean-exponential floor, so it reads unmistakably as "no image at all"
// rather than as a suppression figure.
//
// The LEVEL-domain guards elsewhere in this file keep their own 1e-12 on
// purpose. Those quantities are amplitudes near 0.1-1.0, where 1e-12 is
// already 240 dB down and cannot reach the result.
static constexpr double kSidebandEps = 1e-30;

// ── What the TXA build is entitled to assert about a subject it cannot
//    measure ─────────────────────────────────────────────────────────────
//
// This is a regression threshold on the computed ratio, not a measurement
// of true suppression or an uncertainty bound. It can detect an image that
// rises far enough above numerical error to reduce the ratio below 140 dB.
// Passing does not prove that the image is unresolved: a resolvable 150 dB
// ratio passes too, below the 158 dB diagnostic floor. Correlation error can
// either increase or decrease the ratio, as documented above.
//
// WHERE 140 COMES FROM, and it is not from any TXA run -- a bound copied off a
// run is a test that agrees with itself:
//
//   24 dB ABOVE THE INCUMBENT'S BEST MEASURED POINT ANYWHERE IN THIS FILE
//   (115.75 dB, USB at 1500 Hz in the sweep below; its 1 kHz figure of
//   87.15 dB is the one usually quoted and is not its ceiling). That keeps the
//   old 100.0's policy -- "TXA's worst asserted point still beats the
//   incumbent's best one" -- with the incumbent's real ceiling in it.
//
//   19 dB BELOW THE WORST STRUCTURED-CAPTURE FLOOR measured above (158.6 dB).
//   A gate at or near the floor is not stronger, it is FLAKY: the floor moves
//   between 158 and 334 dB with how much structure a capture happens to carry,
//   so a line drawn through that band goes red on a capture's harmonics rather
//   than on the modulator.
static constexpr double kFloorLimitedSuppDb = 140.0;

// ── The opposite-sideband floor this build is held to ────────────────────
//
// THE ONE NUMBER THE MIGRATION WAS MADE ON, so it is stated here once and
// asserted rather than printed.
//
// The phasing modulator measures 22 dB at 150 Hz on the {150, 3000} DIGU/DIGL
// passband and 87 dB at 1 kHz on {300, 2700}. TXA measures past this
// instrument's numerical floor at both. The migration's whole return is the
// LOW EDGE: at 1 kHz the incumbent is already against EP2's signed-16-bit
// wire, which quantises at about 96 dB (MetisProtocol.cpp, ep2WriteTxIq), so
// TXA's advantage there is below the wire and unusable. At 150 Hz, in the
// modes WSJT-X transmits in, there are seventy decibels the wire could carry
// and 255 taps do not fill.
//
// kMinSuppDb is set ABOVE THE PHASING MODULATOR'S BEST MEASURED POINT in the
// TXA build, on wdsp_channel_test's reasoning: the assertion then reads "TXA's
// worst asserted point still beats the incumbent's best one". If a future
// change brings TXA near the chain it replaced, the migration has lost the
// argument it was made on, and this is the line that says so.
//
// kMinLowEdgeSuppDb is the SAME floor in the TXA build and a characterisation
// bound in the phasing build -- the 22.06 dB #5741 measured, rounded down. It
// is deliberately not "30 dB like the others": 255 taps cannot do 30 dB at
// 150 Hz and never could, and a floor the incumbent cannot meet would be a
// failing test rather than a record of why it was replaced.
//
// Both TXA assertions use the same computed-ratio regression threshold.
// The reported ratios can exceed the empirical numerical floor; that does
// not make them bounds on true suppression. See kFloorLimitedSuppDb for the
// distinction between the 140 dB gate and the 158 dB diagnostic marker.
#if AETHER_HL2_TX_TXA
static constexpr double kMinSuppDb = kFloorLimitedSuppDb;
static constexpr double kMinLowEdgeSuppDb = kFloorLimitedSuppDb;
static constexpr const char* kModulator = "wdsp-txa";
#else
static constexpr double kMinSuppDb = 30.0;
static constexpr double kMinLowEdgeSuppDb = 20.0;
static constexpr const char* kModulator = "phasing";
#endif

// Run `seconds` of a pure audio tone through the modulator and collect the IQ.
// `faults` reports blocks the modulator could not place on the wire.
static std::vector<std::complex<float>> modulateOnce(WdspChannel::Mode mode,
                                                 double toneHz, double amplitude,
                                                 double micGain, double seconds,
                                                 float* lastMicPeak,
                                                 bool alc,
                                                 const double* passband,
                                                 TxAudioSource source,
                                                 unsigned long long* faults)
{
    Hl2TxDsp tx;
    TxTestAuthority authority;
    Hl2TxDsp::Config cfg;
    cfg.mode = mode;
    // Optional explicit passband. Hl2Backend pushes a sign-correct, mode-derived
    // one; the struct default is a positive 300..2700 for every mode, so without
    // this the sideband/passband interaction is never exercised at all.
    if (passband) {
        cfg.filterLowHz  = passband[0];
        cfg.filterHighHz = passband[1];
    }
    // ALC OFF by default in these tests. Every assertion below except the ALC's
    // own measures a fixed relationship between input and output, and an ALC
    // exists precisely to break fixed relationships.
    cfg.alcEnabled = alc;
    std::string err;
    if (!tx.configure(cfg, &err)) {
        std::fprintf(stderr, "FAIL: configure: %s\n", err.c_str());
        ++g_failures;
        return {};
    }
    tx.setMicGain(micGain);

    std::vector<std::complex<float>> out;
    QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                     [&](const std::vector<std::complex<float>>& iq) {
        out.insert(out.end(), iq.begin(), iq.end());
    });
    if (lastMicPeak) {
        QObject::connect(&tx, &Hl2TxDsp::micPeak, &tx,
                         [lastMicPeak](float db) { *lastMicPeak = db; });
    }

    const int fs = cfg.inputSampleRateHz;
    const int total = static_cast<int>(seconds * fs);
    std::vector<float> audio(static_cast<std::size_t>(total));
    for (int n = 0; n < total; ++n)
        audio[static_cast<std::size_t>(n)] =
            static_cast<float>(amplitude * std::sin(2.0 * M_PI * toneHz * n / fs));

    // Feed in realistic chunks rather than one giant block, at the cadence the
    // live caller feeds them (see feed()).
    constexpr std::size_t kChunk = 240;
    for (std::size_t off = 0; off < audio.size(); off += kChunk) {
        const std::size_t n = std::min(kChunk, audio.size() - off);
        feed(tx, std::vector<float>(audio.begin() + static_cast<std::ptrdiff_t>(off),
                                    audio.begin() + static_cast<std::ptrdiff_t>(off + n)),
             source, authority.context);
    }
    *faults = tx.modulatorFaultBlocks();
    return out;
}

// ── The same run, retried if the modulator was STARVED ──────────────────────
//
// EVERY block must reach the wire or the capture is not phase-continuous and
// nothing measured from it means anything: a dropped block does not merely lose
// samples, fexchange2 slips the output stream by a whole DSP buffer
// permanently, so a correlation across the seam averages two time origins.
//
// Zero faults is guaranteed in the phasing build -- arithmetic cannot starve --
// and is a real risk in the TXA build on a SHARED machine. It is not
// hypothetical: measured under a load average of 252, this file and
// wdsp_channel_test both starved, and the sideband assertions then failed
// saying the chain had regressed when what had happened was that eight other
// compiler jobs were running.
//
// Retrying belongs HERE and would be wrong inside Hl2TxDsp: pacing is the
// caller's responsibility, and a sleep-and-retry on the audio I/O thread would
// hide a timing fault behind a spin -- the same class of mistake as the silent
// failure this whole build flag exists to avoid. The runtime path counts and
// logs the fault instead. A test is free to try again; a transmitter is not.
//
// Three attempts, and the retry is PRINTED, because a run that needs retrying
// on an idle machine is evidence about the chain rather than about the machine.
static std::vector<std::complex<float>> modulate(WdspChannel::Mode mode,
                                                 double toneHz, double amplitude,
                                                 double micGain, double seconds,
                                                 float* lastMicPeak = nullptr,
                                                 bool alc = false,
                                                 const double* passband = nullptr,
                                                 TxAudioSource source =
                                                     TxAudioSource::Microphone)
{
    std::vector<std::complex<float>> out;
    unsigned long long faults = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        out = modulateOnce(mode, toneHz, amplitude, micGain, seconds,
                           lastMicPeak, alc, passband, source, &faults);
        if (faults == 0) {
            return out;
        }
        if (attempt < 2) {
            std::fprintf(stderr,
                     "starved at %.0f Hz: %llu blocks did not reach the wire "
                     "-- retrying (attempt %d of 3)\n",
                     toneHz, faults, attempt + 2);
        }
    }
    if (faults != 0) {
        std::fprintf(stderr, "STARVATION: modulatorFaultBlocks=%llu; capture is invalid for spectral assertions\n", faults);
    }
    check(faults == 0,
          "every modulator block reached the wire (capture is contiguous)");
    return out;
}

// ── The same retry, for the cases that build their own modulator ────────────
//
// modulate() above covers the spectral cases. The three level cases below feed
// a hand-built Hl2TxDsp on their own schedule -- a swept level, a held pause --
// so they cannot go through it, and they starve in exactly the same way. It is
// not theoretical: run in-suite under `ctest -j4`, the limiter-release case
// dropped 1 of 132 blocks and its swept tail read 1.02 dB against a 1.00 dB
// tolerance. That is a missing block of audio, not a latching limiter, and
// reading it as the second is the whole failure mode this file exists to catch.
//
// `body` returns the modulator's fault count and its measurements; the checks
// stay OUTSIDE, so a starved attempt is retried rather than recorded as a
// failure. Three attempts, printed.
template <typename Run, typename F>
static Run retryIfStarved(const char* what, F&& body)
{
    Run run{};
    for (int attempt = 0; attempt < 3; ++attempt) {
        run = body();
        if (run.faults == 0) {
            return run;
        }
        if (attempt < 2) {
            std::fprintf(stderr,
                     "starved in %s: %llu blocks did not reach the wire "
                     "-- retrying (attempt %d of 3)\n",
                     what, run.faults, attempt + 2);
        }
    }
    if (run.faults != 0) {
        std::fprintf(stderr, "STARVATION: modulatorFaultBlocks=%llu in %s; capture is invalid\n", run.faults, what);
    }
    check(run.faults == 0,
          "every modulator block reached the wire in this level case");
    return run;
}

int main(int argc, char** argv)
{
    // Hl2TxDsp::processAudioBlock carries its admitting context; these cases
    // are about levelling, so a permanently-valid authority is the inert
    // constant that leaves the source tag as the only variable. Inner scopes
    // that need their own lifetime shadow this one.
    TxTestAuthority authority;
    QCoreApplication app(argc, argv);
    constexpr double kFsOut = 48000.0;
    constexpr double kTone = 1000.0;

    // ---- the build flag is plumbed, and says so ----
    //
    // A flag that is only ever compiled one way is not a flag. This is the
    // check that the CMake option reached the code rather than defaulting
    // silently, and it is why AETHER_HL2_TX_TXA is defined in BOTH directions
    // (0 or 1) instead of only when it is on: "never plumbed" and "off" would
    // otherwise be the same state.
    {
        std::fprintf(stderr, "modulator: %s (AETHER_HL2_TX_TXA=%d)\n",
                     Hl2TxDsp::modulatorName(), AETHER_HL2_TX_TXA);
        check(std::string(Hl2TxDsp::modulatorName()) == kModulator,
              "the compiled modulator is the one the build asked for");
        Hl2TxDsp probe;
        std::string err;
        check(probe.configure(Hl2TxDsp::Config{}, &err),
              "the modulator configures at the shipped defaults");
        // -1 in the phasing build and a real WDSP id in the TXA build. The
        // health snapshot reports this, which is what lets a transmit bug
        // report say which chain produced the signal.
        const int id = probe.wdspChannelId();
        std::fprintf(stderr, "modulator wdsp channel id: %d\n", id);
        check(AETHER_HL2_TX_TXA ? (id >= 0) : (id < 0),
              "wdspChannelId() reports a channel only where one exists");
    }

    // ---- rate conversion ----
    {
        const auto iq = modulate(WdspChannel::Mode::Usb, kTone, 0.5, 1.0, 0.5);
        check(!iq.empty(), "USB modulation produces IQ");
        double mx = 0.0;
        for (const auto& v : iq) mx = std::max(mx, static_cast<double>(std::abs(v)));
        std::fprintf(stderr, "diag: %zu IQ samples, max |IQ| = %.9f\n", iq.size(), mx);
        // 0.5 s of 24 kHz audio -> ~0.5 s of 48 kHz IQ. WDSP's filter latency
        // eats a little, so allow a generous margin; what matters is the 2:1.
        check(iq.size() > 18000 && iq.size() < 26000,
              "24 kHz audio in -> ~48 kHz IQ out (2:1 rate conversion)");
    }

    // Reset must discard the BEGINNING of the next over, including after a
    // receive interval with no processing calls. No settling prefix is omitted.
    {
        Hl2TxDsp tx;
        Hl2TxDsp::Config cfg;
        cfg.alcEnabled = false;
        std::string err;
        const bool configured = tx.configure(cfg, &err);
        check(configured, "reset regression configures");
        if (configured) {
            tx.setMicGain(1.0);
            std::vector<std::complex<float>> captured;
            QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                             [&captured](const auto& iq) {
                captured.insert(captured.end(), iq.begin(), iq.end());
            });
            const int channelId = tx.wdspChannelId();
            std::vector<float> audio(512);
            auto clockBlock = [&] {
                tx.processAudioBlock(audio, TxAudioSource::Microphone, authority.context);
                if (AETHER_HL2_TX_TXA) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(22));
                }
            };
            for (const int gapMs : {0, 500, 0}) {
                captured.clear();
                for (int block = 0; block < 32; ++block) {
                    for (int n = 0; n < 512; ++n) {
                        audio[static_cast<std::size_t>(n)] = static_cast<float>(
                            0.5 * std::sin(2.0 * M_PI * kTone * (block * 512 + n)
                                           / cfg.inputSampleRateHz));
                    }
                    clockBlock();
                }
                double tonePeak = 0.0;
                for (const auto& sample : captured) {
                    tonePeak = std::max(tonePeak, static_cast<double>(std::abs(sample)));
                }
                check(tonePeak > 0.1, "each over resumes producing tone after reset");
                tx.reset();
                tx.reset(); // Repeated unkey is harmless.
                check(tx.isConfigured() && tx.wdspChannelId() == channelId,
                      "reset preserves configuration and the allocated channel");
                std::this_thread::sleep_for(std::chrono::milliseconds(gapMs));
                captured.clear();
                std::fill(audio.begin(), audio.end(), 0.0f);
                for (int block = 0; block < 16; ++block) {
                    clockBlock();
                }
                double stalePeak = 0.0;
                for (const auto& sample : captured) {
                    stalePeak = std::max(stalePeak, static_cast<double>(std::abs(sample)));
                }
                std::fprintf(stderr, "reset [%s] gap=%d ms: head-inclusive peak %.9g, faults %llu\n",
                             tx.modulatorName(), gapMs, stalePeak, tx.modulatorFaultBlocks());
                check(captured.size() == 16 * 1024,
                      "post-reset silence preserves the complete output stream");
                check(stalePeak < 1e-6, "reset drops the previous over from the FIRST output sample");
                check(tx.modulatorFaultBlocks() == 0, "reset regression has no underruns");
            }
        }
    }

    // ---- A MODE CHANGE ON A RUNNING MODULATOR MOVES THE SIDEBAND ----
    //
    // THE ONE PATH EVERY OTHER CASE IN THIS FILE MISSES. Each case above
    // constructs a fresh Hl2TxDsp already in the mode it measures, so the
    // signed passband is baked in by buildModulator() at open time and
    // applyModeAndFilter() is never reached. Its two callers are setMode() and
    // setFilter() on an ALREADY OPEN channel, which nothing exercised.
    //
    // That is exactly the path Hl2TxDsp::applyModeAndFilter warns about in its
    // own comment: in the TXA build SetTXAMode does NOT choose a sideband --
    // TXASetupBPFilters gives TXA_LSB and TXA_USB the identical
    // CalcBandpassFilter call, so the sideband rides entirely on the SIGN of
    // the passband. A mode change pushing only the mode leaves the transmitter
    // on the previous sideband with the readout saying otherwise, and it is
    // invisible from inside this application because the panadapter reads the
    // same wire order.
    //
    // Found by mutation, not by inspection: deleting the setFilter() call from
    // applyModeAndFilter() left this whole file green.
    {
        Hl2TxDsp tx;
        Hl2TxDsp::Config cfg;
        cfg.mode = WdspChannel::Mode::Usb;
        cfg.alcEnabled = false;
        std::string err;
        if (tx.configure(cfg, &err)) {
            tx.setMicGain(1.0);
            std::vector<std::complex<float>> cap;
            QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                             [&cap](const std::vector<std::complex<float>>& iq) {
                cap.insert(cap.end(), iq.begin(), iq.end());
            });
            const int fs = cfg.inputSampleRateHz;
            constexpr std::size_t kChunk = 240;
            auto run = [&](double seconds) {
                const int total = static_cast<int>(fs * seconds);
                std::vector<float> chunk(kChunk);
                for (int off = 0; off + static_cast<int>(kChunk) <= total;
                     off += static_cast<int>(kChunk)) {
                    for (std::size_t n = 0; n < kChunk; ++n)
                        chunk[n] = static_cast<float>(
                            0.5 * std::sin(2.0 * M_PI * kTone
                                           * (off + static_cast<int>(n)) / fs));
                    feed(tx, chunk, TxAudioSource::Microphone, authority.context);
                }
            };
            auto sidebandDb = [&]() {
                const double up = binPower(cap, +kTone, kFsOut, true);
                const double lo = binPower(cap, -kTone, kFsOut, true);
                return 20.0 * std::log10((lo + kSidebandEps) / (up + kSidebandEps));
            };

            run(1.0);
            const double usbDb = sidebandDb();     // + means energy on the LOWER wire bin
            std::fprintf(stderr, "mode change: as USB %.2f dB (lower-over-upper)%s\n",
                         usbDb, floorMark(std::fabs(usbDb)));
            check(usbDb > 0.0,
                  "mode change: a fresh USB modulator puts energy on the LOWER wire bin");

            // THE CHANGE ITSELF, on the open channel.
            tx.setMode(WdspChannel::Mode::Lsb);
            cap.clear();
            run(1.0);
            const double lsbDb = sidebandDb();
            std::fprintf(stderr, "mode change: after setMode(Lsb) %.2f dB "
                                 "(lower-over-upper)%s\n",
                         lsbDb, floorMark(std::fabs(lsbDb)));
            check(lsbDb < 0.0,
                  "mode change: setMode(Lsb) on a RUNNING modulator moves the "
                  "sideband to the UPPER wire bin");
            check(std::fabs(lsbDb - usbDb) > 20.0,
                  "mode change: and it actually moves -- the two sidebands are "
                  "not the same measurement");
        } else {
            std::fprintf(stderr, "FAIL: mode-change case configure: %s\n", err.c_str());
            ++g_failures;
        }
    }

    // ---- USB puts the tone ABOVE the carrier ----
    {
        const auto iq = modulate(WdspChannel::Mode::Usb, kTone, 0.5, 1.0, 1.0);
        if (!iq.empty()) {
            const double upper = binPower(iq, +kTone, kFsOut, true);
            const double lower = binPower(iq, -kTone, kFsOut, true);
            const double ratioDb =
                20.0 * std::log10((upper + kSidebandEps) / (lower + kSidebandEps));
            // The bins print in scientific notation because an image at 1e-17
            // and an image of exactly zero are the same six decimal places, and
            // telling them apart is the whole point of this block. Suppression
            // prints POSITIVE, which is what the word means and what the
            // assertion below tests; it used to print as a negative ratio.
            std::fprintf(stderr, "USB: +1kHz %.9e, -1kHz %.9e, suppression %.2f dB%s\n",
                         upper, lower, -ratioDb, floorMark(-ratioDb));
            // The WIRE-FACING sign, and it is the same assertion in both
            // builds although the two modulators reach it by opposite routes:
            // the phasing modulator emits the analytic signal and CONJUGATES
            // it, a TXA channel is handed a signed passband and must NOT be
            // conjugated. Either one alone, or both, puts the transmitter on
            // the wrong sideband -- invisible from inside this application,
            // because the panadapter reads the same wire order.
            check(lower > upper,
                  "USB: wire-facing IQ puts energy on the LOWER side (conjugated)");
            check(-ratioDb > kMinSuppDb, "USB: opposite sideband suppressed");
        }
    }

    // ---- LSB puts it BELOW ----
    {
        const auto iq = modulate(WdspChannel::Mode::Lsb, kTone, 0.5, 1.0, 1.0);
        if (!iq.empty()) {
            const double upper = binPower(iq, +kTone, kFsOut, true);
            const double lower = binPower(iq, -kTone, kFsOut, true);
            const double ratioDb =
                20.0 * std::log10((lower + kSidebandEps) / (upper + kSidebandEps));
            std::fprintf(stderr, "LSB: +1kHz %.9e, -1kHz %.9e, suppression %.2f dB%s\n",
                         upper, lower, -ratioDb, floorMark(-ratioDb));
            check(upper > lower,
                  "LSB: wire-facing IQ puts energy on the UPPER side (conjugated)");
            check(-ratioDb > kMinSuppDb, "LSB: opposite sideband suppressed");
        }
    }

    // ── Opposite-sideband suppression at the DIGU/DIGL LOW EDGE ────────────
    //
    // THIS IS THE ASSERTION THE WHOLE TXA MIGRATION WAS MADE ON, and it used
    // to be a diagnostic print with no assertion at all.
    //
    // It measures the {150, 3000} passband Hl2Backend pushes for DIGU and
    // DIGL, at the frequencies where 255 taps run out of transition room. The
    // phasing modulator reads 22 dB at 150 Hz there; a TXA channel reads past
    // this instrument's numerical floor. No other row in this file separates
    // the two chains by anything that survives EP2's signed-16-bit wire: at
    // 1 kHz the incumbent already measures 87 dB against a wire that quantises
    // near 96 dB, so the mid-band rows above cannot carry the argument even
    // though the numbers there are larger.
    //
    // It is DIGU and DIGL and nothing else. That is the modes WSJT-X transmits
    // in, and the reason the case for this migration is narrower than the
    // study that proposed it claimed.
    //
    // The floor differs per build on purpose -- see kMinLowEdgeSuppDb. In the
    // phasing build this is #5741's characterisation bound and records why the
    // chain was replaced; in the TXA build it is the same computed-ratio
    // regression threshold as the mid-band rows (kFloorLimitedSuppDb).
    // Passing does not establish true suppression or an unresolved image.
    //
    // THAT SENTENCE USED TO SAY "the same 100 dB ... which sits above the
    // phasing modulator's BEST measured point anywhere", AND IT WAS NOT TRUE
    // ON THIS TREE. The phasing sweep below reaches 115.75 dB at 1500 Hz on
    // {300, 2700}; its 87 dB at 1 kHz is the figure usually quoted and is not
    // its ceiling. The claim was checked against the wrong row of the sweep
    // this very file prints. kFloorLimitedSuppDb is 140.0 and the claim is
    // true of it.
    {
        const double diguBand[2] = {150.0, 3000.0};
        for (const double toneHz : {150.0, 200.0, 300.0, 1000.0}) {
            const auto iq = modulate(WdspChannel::Mode::Digu, toneHz, 0.5, 1.0, 1.0,
                                     nullptr, false, diguBand);
            if (iq.empty()) {
                check(false, "DIGU low-edge sweep produced IQ");
                continue;
            }
            const double wanted = binPower(iq, -toneHz, kFsOut, true);
            const double image  = binPower(iq, +toneHz, kFsOut, true);
            const double suppDb =
                20.0 * std::log10((wanted + kSidebandEps) / (image + kSidebandEps));
            std::fprintf(stderr,
                         "%s DIGU {150,3000} tone %.0f Hz: "
                         "wanted %.9f  image %.9e  suppression %.2f dB%s\n",
                         kModulator, toneHz, wanted, image, suppDb,
                         floorMark(suppDb));
            check(wanted > image,
                  "DIGU low edge: the sideband is on the expected wire bin");
            check(suppDb > kMinLowEdgeSuppDb,
                  "DIGU low edge: opposite sideband suppressed");
        }
    }

    // ---- DIGU/DIGL transmit on the same sidebands as USB/LSB ----
    //
    // WSJT-X transmits in DIGU. These modes were never covered here, and the
    // sideband is the whole question: an FT8 signal on the wrong sideband is
    // both un-decodable and 3 kHz away from where the operator believes it is.
    //
    // Each is checked with the exact passband Hl2Backend now pushes for that
    // mode, which is the combination that actually ships -- the assertions above
    // only ever ran against the struct default, so the passband's effect on the
    // sideband was untested until this block existed.
    {
        // POSITIVE for every mode. This is the TX convention, and it is the
        // opposite of RX: here the MODE carries the sideband and the bandpass is
        // an audio-domain magnitude. Feeding TX the RX table's signed pairs put
        // LSB and DIGL on the upper sideband -- caught by this very block before
        // it shipped, which is why the two tables are separate in Hl2Backend.
        //
        // Read from production for the same reason the sweep below does: these
        // were hand-typed until #5741, and the label this block PRINTS is built
        // from them, so a widened window upstream would have had the test
        // reporting a passband the modulator was not using.
        const auto diguDefault = AetherSDR::hl2::defaultTxPassbandForModeName("DIGU");
        const auto usbDefault  = AetherSDR::hl2::defaultTxPassbandForModeName("USB");
        const double diguBand[2] = {double(diguDefault.first), double(diguDefault.second)};
        const double diglBand[2] = {double(diguDefault.first), double(diguDefault.second)};
        const double usbBand[2]  = {double(usbDefault.first),  double(usbDefault.second)};
        const double lsbBand[2]  = {double(usbDefault.first),  double(usbDefault.second)};

        struct Case { const char* name; WdspChannel::Mode mode; const double* band;
                      bool wireUpper; };
        // wireUpper mirrors the USB/LSB assertions above: the wire order is
        // conjugated, so a USB-family mode lands on the LOWER wire bin.
        const Case cases[] = {
            {"USB",  WdspChannel::Mode::Usb,  usbBand,  false},
            {"DIGU", WdspChannel::Mode::Digu, diguBand, false},
            {"LSB",  WdspChannel::Mode::Lsb,  lsbBand,  true},
            {"DIGL", WdspChannel::Mode::Digl, diglBand, true},
        };

        for (const Case& c : cases) {
            const auto iq = modulate(c.mode, kTone, 0.5, 1.0, 1.0,
                                     nullptr, false, c.band);
            if (iq.empty()) {
                check(false, "modulation produced IQ for this mode");
                continue;
            }
            const double upper = binPower(iq, +kTone, kFsOut, true);
            const double lower = binPower(iq, -kTone, kFsOut, true);
            const double wanted = c.wireUpper ? upper : lower;
            const double other  = c.wireUpper ? lower : upper;
            const double suppDb =
                20.0 * std::log10((wanted + kSidebandEps) / (other + kSidebandEps));
            std::fprintf(stderr,
                         "%-4s (passband %.0f..%.0f): +1kHz %.9e, -1kHz %.9e, "
                         "suppression %.2f dB%s\n",
                         c.name, c.band[0], c.band[1], upper, lower, suppDb,
                         floorMark(suppDb));
            check(wanted > other, "sideband is correct for this mode");
            check(suppDb > kMinSuppDb, "opposite sideband suppressed");
        }
    }

#if !AETHER_HL2_TX_TXA
    // ---- PHASING BUILD ONLY, and the reason is the block's own ----
    //
    // The evidence below is "these modes are BIT-IDENTICAL to USB", which is
    // true because Hl2TxDsp::setMode()'s only reader is isLowerSideband(). A
    // TXA channel does not work that way: it implements AM, SAM, FM and DSB as
    // real WDSP transmit modes, and refuses WBFM outright ("WDSP TX does not
    // define a WBFM mode"). So in the TXA build every assertion here fails --
    // seven of them -- while nothing is wrong.
    //
    // The block says what to do about that in its own words: "If someone later
    // teaches this chain a real AM or FM modulator, this block FAILS, which is
    // the point: the failure is the reminder to take that mode back off the
    // receive-only list." That reminder is hereby taken, and it is NOT this
    // PR's to act on. Whether TXA makes AM and FM genuinely transmittable on a
    // Hermes-Lite 2 is a CAPABILITY decision -- it changes what
    // Hl2Backend::capabilities() declares and what
    // RadioModel::refuseKeyInReceiveOnlyMode() will let an operator key -- and
    // it belongs with the TXA migration rows (#5678 1.2, 6.2, 6.3), behind a
    // flag that is not yet on by default and a modulator nobody has keyed on
    // the air.
    //
    // Guarding rather than deleting, because the phasing build is what ships
    // today and this is its evidence. Found by building the integration branch:
    // #5680 landed this block on main after this branch was cut, so neither
    // change could see the collision alone.
    // ---- THE MODES Hl2Backend DECLARES RECEIVE-ONLY ARE BIT-IDENTICAL TO USB ----
    //
    // This is the evidence behind `Hl2Backend::capabilities`'s
    // `receiveOnlyModes` list, and it is deliberately stronger than asserting
    // that a list contains some strings. A list can drift from the modulator;
    // this cannot.
    //
    // Hl2TxDsp::setMode() stores the mode and the ONLY reader is
    // isLowerSideband(), which returns true for Lsb/Cwl/Digl and false for
    // everything else. So AM, SAM, DSB, FM, WBFM and DRM do not take some
    // degraded AM or FM path -- they take the USB path exactly, and what goes on
    // the air is single-sideband suppressed carrier while the mode indicator
    // says otherwise.
    //
    // If someone later teaches this chain a real AM or FM modulator, this block
    // FAILS, which is the point: the failure is the reminder to take that mode
    // back off the receive-only list.
    //
    // SIX enumerators here cover EIGHT declared strings: Hl2Backend's
    // modeFromString() maps NFM onto Mode::Fm and WFM onto Mode::Wbfm, so those
    // two spellings have no enumerator of their own to modulate. That the
    // DECLARATION still carries both — the guard compares the string the slice
    // holds, not the enumerator — is asserted in hl2_family_transition_test,
    // which reads capabilities() off a live backend. This file cannot see it.
    {
        const double usbBand[2] = {300.0, 2700.0};
        const auto reference = modulate(WdspChannel::Mode::Usb, kTone, 0.25,
                                        1.0, 1.0, nullptr, false, usbBand);
        check(!reference.empty(), "USB reference modulation produced IQ");

        struct DeclaredReceiveOnly { const char* name; WdspChannel::Mode mode; };
        const DeclaredReceiveOnly declared[] = {
            {"AM",   WdspChannel::Mode::Am},
            {"SAM",  WdspChannel::Mode::Sam},
            {"DSB",  WdspChannel::Mode::Dsb},
            {"FM",   WdspChannel::Mode::Fm},
            {"WBFM", WdspChannel::Mode::Wbfm},
            {"DRM",  WdspChannel::Mode::Drm},
        };

        for (const DeclaredReceiveOnly& d : declared) {
            const auto iq = modulate(d.mode, kTone, 0.25, 1.0, 1.0,
                                     nullptr, false, usbBand);
            check(iq.size() == reference.size(),
                  "declared receive-only mode produced the same sample count as USB");
            bool identical = (iq.size() == reference.size());
            std::size_t firstDiff = 0;
            for (std::size_t k = 0; identical && k < iq.size(); ++k) {
                if (iq[k] != reference[k]) { identical = false; firstDiff = k; }
            }
            std::fprintf(stderr,
                         "%-4s vs USB: %s\n", d.name,
                         identical ? "bit-identical (no distinct modulation)"
                                   : "DIFFERS -- a real modulator now exists");
            if (!identical) {
                std::fprintf(stderr, "  first difference at sample %zu\n", firstDiff);
            }
            check(identical,
                  "this mode is indistinguishable from USB, which is why "
                  "Hl2Backend declares it receive-only");
        }
    }
#endif  // !AETHER_HL2_TX_TXA

    // ---- CHARACTERISATION SWEEP: opposite-sideband suppression vs audio frequency ----
    //
    // WHAT THIS IS FOR. Everything above measures the modulator at ONE audio
    // frequency, 1 kHz, where it is at its best. The number that decides
    // whether this modulator is good enough is not that one: it is the
    // suppression at the LOW EDGE of the passband, and it is much worse. This
    // block measures the whole passband and PRINTS EVERY VALUE, so that the
    // shape of the curve is on the record rather than a single flattering
    // point.
    //
    // WHY THE LOW EDGE. Hl2TxDsp's filters are 255 taps with a Blackman window
    // at 48 kHz (designFilters()). A Blackman-windowed design's transition
    // width is about 11*fs/N, roughly 2 kHz here, so a 150 Hz low edge is
    // nowhere near resolved: the analytic prototype still has real gain at
    // -150 Hz, and that gain IS the opposite sideband. The image ratio is
    // |H(+f)| / |H(-f)| of the analytic filter, and near the low edge those two
    // are not far apart.
    //
    // WHY 150 Hz SPECIFICALLY. Hl2Backend::defaultTxPassbandForMode pushes
    // {150, 3000} for DIGU and DIGL -- the modes WSJT-X transmits in. The voice
    // modes get {300, 2700} and sit in a much better part of the curve. So the
    // weakest DEFAULT in this table is also the case that carries FT8.
    //
    // THE WEAKEST DEFAULT IS NOT THE WEAKEST THING THAT SHIPS. The operator can
    // set the passband directly: Hl2Backend::setTxFilter clamps the low edge to
    // [0, kTxAudioMaxHz - 50] and effectiveTxPassband returns that pair verbatim
    // for USB and LSB, so a 100 Hz eSSB edge -- or a 0 Hz one -- reaches this
    // modulator without any mode being unusual. The last two rows of bands[]
    // measure exactly that, and they are worse than any default:
    //
    //                    100 Hz   150 Hz   300 Hz   500 Hz
    //   eSSB 100..4000    11.86    18.21    42.15    78.61
    //   wide    0..4000     7.87    12.09    27.73    68.05
    //
    // Hl2Backend's own eSSB comment asserts that "the 255-tap Blackman prototype
    // keeps a usable skirt across that range". These two rows are the first
    // measurement of that claim, and what they say is that "usable" has to mean
    // something weaker than the voice figure: 11.86 dB at the edge the operator
    // is invited to choose. Whether that is acceptable is a judgement for the
    // thread, not for this test -- the test's job is that the number now exists.
    // Reported by aethersdr-agent on #5741, reproduced here to 0.01 dB.
    //
    // WHAT THE BOUNDS ARE AND ARE NOT.
    //
    //   THE FLOORS ARE PER-ROW, because they are properties of the PASSBAND.
    //   A single pair hoisted over the table would read as a statement about
    //   the modulator, and the eSSB row disproves that reading directly: same
    //   modulator, same taps, same window, 11.86 dB instead of 22.06 because
    //   the edge moved. What sets suppression at a tone is how deep inside the
    //   skirt that tone sits.
    //
    //   The low-edge floor is deliberately far below what the low edge
    //   measures. That figure is a BASELINE BEING RECORDED, not a target being
    //   enforced. Asserting it tightly would freeze today's weakness into the
    //   test and the next chain would have to be bug-compatible with it.
    //
    //   The settled floor is the half that discriminates. Above kSettledFromHz
    //   the windowed design has fully settled and the suppression is set by the
    //   window's sidelobe floor rather than by the transition. A shorter filter
    //   or a window with worse sidelobes shows up here immediately; a floor at
    //   the low edge alone does not see it. Every bound was chosen from the
    //   table this block prints, with margin, on the code as it stands -- see
    //   the commit message.
    //
    // THE BASELINE THIS RECORDS, on the tree it was written against:
    //
    //   DIGU/DIGL {150, 3000}:  150 Hz 22.06 dB   200 Hz 30.58 dB
    //                           300 Hz 52.26 dB   1 kHz  83.10 dB
    //   USB/LSB   {300, 2700}:  300 Hz 72.43 dB   500 Hz 76.26 dB
    //                           1 kHz  87.15 dB
    //
    // Two of those were, until this block ran, DERIVED AND NEVER MEASURED --
    // the 22 dB and 30.6 dB low-edge figures. They hold: the measurement lands
    // on 22.06 and 30.58.
    //
    // THE 1 kHz VOICE FIGURE HAS INDEPENDENT PROVENANCE, AND IT IS NOT A
    // HARDWARE MEASUREMENT. An earlier revision of this paragraph called it
    // one. It is not, and the distinction is the whole reason to state the
    // provenance rather than the number:
    //
    //   Run `d87-ssb-tone-ab`. The peer is `hpsdrsim` on 127.0.0.1 -- NETWORK
    //   loopback to a simulator. No RF anywhere, no transmitter keyed, no
    //   radio in the path at any point. The instrument is
    //   `streams/bench-runner/tools/ep2_sideband_ratio.py`, reading the EP2
    //   wire: the IQ this backend actually handed to the socket.
    //
    //   Steady 1 kHz tone, TWELVE full-level overs, 87.15-87.19 dB with a
    //   0.04 dB spread. (Sixteen overs were run. The other four are at
    //   -20 dBFS and read 87.13 and 85.98, outside the range, so quoting
    //   sixteen against these bounds would be quoting a spread that does not
    //   exist.)
    //
    // This sweep's USB row lands at 87.15 dB, inside that range.
    //
    // WHAT THE AGREEMENT ACTUALLY BUYS, since neither measurement touches a
    // radio: d87 reads the wire AFTER MetisProtocol::ep2WriteTxIq has packed
    // the samples into signed 16-bit, and this block reads the modulator's
    // output BEFORE it. They agree to better than 0.01 dB at 1 kHz. So the
    // wire packing contributes nothing measurable at 1 kHz -- which is a real
    // result, and one this block could not reach on its own, because it never
    // links MetisProtocol.
    //
    // Nor is the agreement confined to 1 kHz. The same bench family swept three
    // tones and recorded {500: 76.26, 1000: 87.15, 2000: 100.17}; this sweep
    // reads 76.26, 87.15 and 100.15. Agreement to 0.02 dB at the worst of the
    // three.
    //
    // (aethersdr-agent asked, on #5741, where this figure came from: every
    // other number in the block could be re-derived from the tree and this one
    // could not, and a hardware-sounding claim sat oddly beside the block's own
    // "nothing here touches a radio". The suspicion was right on both counts --
    // the count was wrong AND it was never hardware. A comment that cannot be
    // checked ages into a fact nobody can retire, which is exactly what this
    // one had started to do.)
    //
    // AND THE FLOAT ARITHMETIC IS NOT THE LIMIT ANYWHERE IN THE TABLE. An
    // independent double-precision evaluation of the same filter design agrees
    // with every row of this sweep to better than 0.02 dB, including the rows
    // above 100 dB. The float taps and the float accumulator in
    // processAudioBlock are therefore not what caps the suppression; the window
    // is. That matters for the migration argument, because it means the ceiling
    // moves if and only if the filter design moves.
    //
    // MUTATION-CHECKED, which is the only thing that makes a passing bound
    // worth anything. Three degradations of designFilters() were tried against
    // this sweep:
    //
    //   kTaps 255 -> 127      DIGU low edge 22.1 -> 7.9 dB, settled 83 -> 29 dB.
    //                         BOTH floors go red.
    //   Blackman -> Hann      low edge IMPROVES (22.1 -> 40.6 dB: the main lobe
    //                         narrows) and the settled band degrades to 63.7 dB.
    //                         The low-edge floor PASSES it. kSettledFloorDb is
    //                         the only thing that catches it.
    //   Blackman -> no window low edge 23.5 dB, settled 33.1 dB. Again the
    //                         low-edge floor passes and kSettledFloorDb catches.
    //   kTaps 255 -> 191      a 25% trim, not a gross one. DIGU low edge 14.0 dB,
    //                         settled 63.4 dB. Both floors go red -- and EVERY
    //                         OTHER ASSERTION IN THIS FILE STILL PASSES it,
    //                         including the 1 kHz >30 dB checks and the 5 kHz
    //                         rejection check. This is the case that shows the
    //                         sweep sees something nothing else here sees.
    //
    // Three of the four plausible degradations are invisible to a low-edge
    // bound, and one of the four is invisible to everything else in this file.
    // That is the whole reason there are two floors rather than one, and it is
    // why a single "worst point in the sweep" assertion would have been
    // decoration.
    //
    // WHOSE CHARACTERISATION THIS IS, AND WHOSE IT IS NOT. Every baseline
    // figure in this comment and every bound in the table below is the PHASING
    // modulator's. Those are real measurements: 7.87 to 115.75 dB, all of it
    // far under the instrument's numerical floor (kInstrumentFloorDb).
    //
    // IN THE TXA BUILD THIS SWEEP CHARACTERISES ALMOST NOTHING, AND THE
    // EXCEPTION IS THE INTERESTING PART. With the settle corrected, every row
    // of the five passbands whose low edge is 100 Hz or above prints a figure
    // at or above the numerical floor -- flagged as such in the table -- so
    // the shape of the curve THERE is this file's double arithmetic and not
    // WDSP's filter. Those rows are UNRESOLVED rather than a curve -- not
    // lower bounds: the correlation error is complex and can oppose the image,
    // so a floor-limited ratio can read high as easily as low.
    //
    // The exception is the 0 Hz low edge, where a TXA skirt is still
    // resolvable and this sweep measures it: on {0, 4000} the 100 Hz row reads
    // 59.37 dB on a wanted bin of 4.995e-01 and an image of 5.372e-04 -- real
    // numbers, four decades clear of the noise -- and the 150 Hz row reads
    // 142.26 dB. Nothing else in this file measures that. It is the one place
    // the TXA rows below are a characterisation and not a bound, and it is
    // exactly where an operator who widens the low edge all the way would
    // live.
    //
    // Elsewhere the TXA sweep checks the computed ratio against a regression
    // threshold. floorMark() separately flags ratios at the empirical
    // numerical floor; neither the gate nor the marker gives an error bound.
    //
    // NOT MEASURED HERE: nothing in this block touches a radio. No transmitter
    // is keyed, no simulator runs, no network socket opens. These are the
    // modulator's own emitted IQ read directly in wire order, which is a
    // stronger instrument than a loopback (two conjugations cancel in a
    // loopback) but a weaker one than an over-the-air measurement with a second
    // receiver, because it cannot see anything the PA or the wire does.
    {
        constexpr double kSweepSeconds   = 0.75;
        constexpr double kSettledFromHz  = 500.0;

        // ── THE SETTLE IS kSettleSamples, THE SAME ONE THE ASSERTED BLOCKS
        //    USE, and until this change it was not.
        //
        // This block declared its own local `kSettle = 4096`. kSettleSamples'
        // comment, two hundred lines up, already said why that is wrong -- the
        // census walks 0, 0, 7.8e-14, 2.2e-04, 4.3e-02 and only settles at
        // block 5, so "a 4096-sample skip lands inside the ramp and the
        // residual amplitude modulation held the image bin at 4.4e-05, an
        // 81 dB figure that is the ENVELOPE, not the filter" -- and then
        // claimed "Applied in BOTH builds", which this block made false. The
        // sweep read 5.9e-05 and 78.41 dB -- the same artefact, within 2.6 dB
        // of the census's own figure for it.
        //
        // WHAT THE SWEEP WAS ACTUALLY MEASURING IN THE TXA BUILD, measured on
        // the tree before this change. Three independent proofs it was WDSP's
        // create_slews T/R mute ramp and not a sideband:
        //
        //   PASSBAND INDEPENDENCE. From 500 Hz up, USB (low edge 300 Hz), DIGU
        //   (150), eSSB (100) and wide (0) printed IDENTICAL figures: 72.39,
        //   75.31, 78.41, 81.90, 84.35, 86.22. A filter's image ratio cannot
        //   be independent of the filter.
        //
        //   A 1/f SIGNATURE, WHICH IS A TRUNCATED TRANSIENT AND NOT A SKIRT.
        //   Slope of the suppression curve across the widest decade each
        //   passband admits (USB 300->2700 Hz, the rest 300->3000 Hz):
        //
        //                 TXA at 4096      the phasing control
        //     USB           19.75              30.55   dB/decade
        //     DIGU          19.77              55.11
        //     eSSB          19.80              72.87
        //     wide          19.80              79.48
        //
        //   The TXA column is 20.00 dB/decade -- an exact 1/f -- to within
        //   0.25 dB, and it is THE SAME SLOPE for four different filters. The
        //   control's slope varies by 49 dB/decade with the passband, because
        //   a real skirt is a property of the filter that made it.
        //
        //   THE CONTROL IS UNMOVED BY THIS CHANGE. The phasing modulator's
        //   priming is its 255-tap delay line filling -- "about 5 ms" by
        //   kSettleSamples' own reckoning, 240 output samples -- so 4096
        //   already cleared it and 12288 clears it by more. The whole 84-row
        //   sweep table is byte-identical either way, DIGU at 150 Hz included
        //   (22.06 dB, which is #5741 exactly).
        //
        // The same cell moves 215.8 dB between the two settles: this sweep's
        // DIGU row at 1 kHz reads 78.41 dB at 4096 and 294.19 dB at 12288.
        //
        // And the cost of leaving it was that the tightest TXA margin in this
        // file was 2.39 dB -- 72.39 dB against the 70.0 dB settled floor --
        // resting on the shape of a mute ramp. A change to create_slews would
        // have turned this sweep red for a reason that has nothing to do with
        // sideband suppression.

        // THE FLOORS BELONG TO THE PASSBAND, NOT TO THE MODULATOR, so each row
        // carries its own. An earlier revision hoisted one pair of constants
        // over the whole table and read as a claim about the modulator; it is
        // not one. Suppression at a given tone is set by how far that tone sits
        // inside the FIR's skirt, so widening the low edge moves the floor and
        // nothing about the modulator has changed. Caught by aethersdr-agent on
        // #5741, who ran the eSSB case and got 11.86 dB -- under the 15.0 a
        // single hoisted floor would have asserted.
        // These values originate in the phasing characterisation. Both builds
        // retain sweepFloorDb, including the resolvable low-edge wide rows.
        // settledFloor() selects the stronger TXA regression threshold.
        struct Band { const char* name; WdspChannel::Mode mode;
                      double lo; double hi; bool wireUpper;
                      double sweepFloorDb; double settledFloorDb; };
        // wireUpper follows the assertions above: the wire order is conjugated,
        // so a USB-family mode lands on the LOWER wire bin.
        //
        // The first four rows are what defaultTxPassbandForMode() produces. The
        // last two are NOT defaults and are not reachable by choosing a mode:
        // they are what Hl2Backend::setTxFilter() admits, which clamps the low
        // edge to [0, kTxAudioMaxHz - 50] and which effectiveTxPassband()
        // returns verbatim for USB and LSB. effectiveTxPassband's own comment
        // names "an operator who widened to 100..4000 for eSSB" as the case it
        // is reasoning about, so this is a documented path rather than a
        // theoretical one -- and until this row it was an unmeasured one.
        // READ FROM PRODUCTION, not re-typed. An earlier revision spelled
        // {300, 2700} and {150, 3000} out by hand, which made the mirror
        // SILENT: widen DIGU's window upstream and this sweep would go on
        // characterising the old passband while the paragraph above still
        // claimed it described what WSJT-X transmits through. The mapping now
        // lives in Hl2TxLevelPolicy.h -- Qt-free, so this test can call the
        // same expression Hl2Backend runs. Caught by aethersdr-agent on #5741.
        const auto voice = AetherSDR::hl2::defaultTxPassbandForModeName("USB");
        const auto digi  = AetherSDR::hl2::defaultTxPassbandForModeName("DIGU");
        const Band bands[] = {
            {"USB",  WdspChannel::Mode::Usb,  double(voice.first), double(voice.second), false, 15.0, 70.0},
            {"LSB",  WdspChannel::Mode::Lsb,  double(voice.first), double(voice.second), true,  15.0, 70.0},
            {"DIGU", WdspChannel::Mode::Digu, double(digi.first),  double(digi.second),  false, 15.0, 70.0},
            {"DIGL", WdspChannel::Mode::Digl, double(digi.first),  double(digi.second),  true,  15.0, 70.0},
            // NOT defaults, and deliberately literal: these are the operator's
            // own edges via Hl2Backend::setTxFilter, so there is no production
            // constant to read. 4000 is kTxAudioMaxHz; 0 and 100 are what the
            // clamp in setTxFilter admits at the bottom.
            {"eSSB", WdspChannel::Mode::Usb,  100.0, 4000.0, false, 10.0, 70.0},
            {"wide", WdspChannel::Mode::Usb,    0.0, 4000.0, false,  5.0, 60.0},
        };
        // The floors above are stated for the passbands production currently
        // returns. If that changes, the floors are no longer the right ones and
        // the sweep should be re-baselined rather than silently re-judged.
        check(voice == std::pair<int, int>{300, 2700},
              "sweep baseline: the USB default passband is still 300..2700");
        check(digi == std::pair<int, int>{150, 3000},
              "sweep baseline: the DIGU default passband is still 150..3000");
        // ── THE SETTLED FLOOR IS NOT THE SAME KIND OF STATEMENT IN THE
        //    TWO BUILDS ──────────────────────────────────────────────────
        //
        // b.settledFloorDb is a real bound on a real measurement: 70 dB is a
        // statement about a 255-tap Blackman design's sidelobes, and the
        // mutation table above is what earns it.
        //
        // TXA uses the stronger computed-ratio regression threshold instead
        // of the phasing bound. It detects regressions that the old threshold
        // missed, but does not assert that the image is unresolved or provide
        // a lower bound on true suppression; see kFloorLimitedSuppDb.
        //
        // Written as a runtime ternary rather than an #if so both arms keep
        // compiling in both builds, the same reason AETHER_HL2_TX_TXA is
        // defined in both directions.
        const auto settledFloor = [](const Band& b) {
            return AETHER_HL2_TX_TXA ? kMinSuppDb : b.settledFloorDb;
        };

        // Integer hertz, so wholeCycles() can null the analysis leakage exactly.
        // 100 Hz and 3000 Hz sit outside the voice passband on purpose: the
        // curve either side of an edge is part of what is being characterised.
        const double tones[] = {100.0,  150.0,  200.0,  250.0,  300.0,  400.0,
                                500.0,  700.0, 1000.0, 1500.0, 2000.0, 2500.0,
                               2700.0, 3000.0};

        std::fprintf(stderr,
            "\n=== TX opposite-sideband characterisation sweep "
            "(%s modulator, ALC off, %.2f s per point) ===\n",
            kModulator, kSweepSeconds);
        std::fprintf(stderr, "%-5s %-11s %7s %13s %13s %9s\n",
                     "mode", "passband", "tone", "wanted", "image", "supp dB");

        for (const Band& b : bands) {
            double worstDb = 1e9;
            double worstHz = 0.0;
            for (const double t : tones) {
                const double band[2] = {b.lo, b.hi};
                const auto raw = modulate(b.mode, t, 0.5, 1.0, kSweepSeconds,
                                          nullptr, false, band);
                const auto iq = wholeCycles(raw, t, kFsOut, kSettleSamples);
                char what[160];
                if (iq.empty()) {
                    std::snprintf(what, sizeof(what),
                                  "sweep %s @ %.0f Hz produced analysable IQ",
                                  b.name, t);
                    check(false, what);
                    continue;
                }
                const double upper = binPower(iq, +t, kFsOut);
                const double lower = binPower(iq, -t, kFsOut);
                const double wanted = b.wireUpper ? upper : lower;
                const double image  = b.wireUpper ? lower : upper;
                const double suppDb =
                    20.0 * std::log10((wanted + kSidebandEps) / (image + kSidebandEps));

                // The dB floors apply IN-BAND only. Below the low edge the
                // filter is attenuating the WANTED signal as hard as the image
                // (at 100 Hz on {150, 3000} the wanted is already 12 dB down),
                // so the ratio there is not a statement about the modulator's
                // sideband quality -- it is a statement about the skirt, and
                // the 5 kHz case below is the assertion that owns that. The
                // out-of-band rows are printed because the shape either side of
                // an edge is part of the characterisation.
                const bool inBand = (t >= b.lo && t <= b.hi);

                // ── AND THE WIRE-BIN CHECK NEEDS A SIGNAL TO CHECK ──────────
                //
                // A row whose WANTED bin has itself been filtered down into the
                // capture's own float noise has no sideband left to have an
                // orientation, and `wanted > image` there is a coin toss. It
                // was decided by a coin until this sweep used the right settle:
                // TXA at 100 Hz on {300, 2700} reads wanted 3.949e-11 against
                // image 4.362e-11 and the check goes RED, while at the old
                // 4096-sample settle the same row read wanted 9.553e-08 --
                // T/R-ramp energy, not signal -- and the check passed on it.
                // Passing on ramp energy is not better than failing on noise;
                // both are the instrument talking to itself.
                //
                // In-band rows always qualify: the wanted bin is at or near the
                // 0.5 drive. Out of band this is what decides. 1e-6 is 54 dB
                // under that drive and ~500x above the largest bin-noise this
                // instrument produces at it (2.1e-9), so it separates "the
                // filter removed the tone" from "the filter left a sideband to
                // check". IT DROPS NOTHING IN THE PHASING BUILD -- the smallest
                // wanted bin anywhere in that build's sweep is 1.97e-2, four
                // decades clear -- so the control keeps every assertion it had.
                constexpr double kResolvableBin = 1e-6;
                const bool orientable = inBand || wanted > kResolvableBin;

                std::fprintf(stderr, "%-5s %4.0f..%-6.0f %7.0f %13.6e %13.6e %9.2f%s%s\n",
                             b.name, b.lo, b.hi, t, wanted, image, suppDb,
                             floorMark(suppDb),
                             orientable ? ""
                                        : "  [wanted bin filtered into the noise:"
                                          " no sideband to orient]");

                if (orientable) {
                    std::snprintf(what, sizeof(what),
                                  "sweep %s @ %.0f Hz: sideband is on the expected wire bin",
                                  b.name, t);
                    check(wanted > image, what);
                }

                if (inBand) {
                    std::snprintf(what, sizeof(what),
                                  "sweep %s @ %.0f Hz: %.2f dB is above the %.0f dB passband floor",
                                  b.name, t, suppDb, b.sweepFloorDb);
                    check(suppDb > b.sweepFloorDb, what);
                }

                if (inBand && t >= kSettledFromHz) {
                    const double floorDb = settledFloor(b);
                    std::snprintf(what, sizeof(what),
                                  "sweep %s @ %.0f Hz: %.2f dB is above the %.0f dB settled floor",
                                  b.name, t, suppDb, floorDb);
                    check(suppDb > floorDb, what);
                }

                // ORIENTABLE ROWS ONLY. A row whose wanted bin is in the
                // noise has no figure to be the worst of: in the TXA build the
                // USB 100 Hz row reads -0.86 dB, and reporting that as "the
                // worst point across the sweep" would put a noise ratio at the
                // top of the summary the reader looks at first.
                if (orientable && suppDb < worstDb) { worstDb = suppDb; worstHz = t; }
            }
            std::fprintf(stderr, "%-5s worst point across the sweep: %.2f dB at %.0f Hz\n",
                         b.name, worstDb, worstHz);
        }
        std::fprintf(stderr, "=== end sweep ===\n\n");
    }

    // ---- audio outside the passband does not get transmitted ----
    {
        // 5 kHz is well above the 2700 Hz TX filter.
        const auto iq = modulate(WdspChannel::Mode::Usb, 5000.0, 0.5, 1.0, 1.0);
        const auto ref = modulate(WdspChannel::Mode::Usb, kTone, 0.5, 1.0, 1.0);
        if (!iq.empty() && !ref.empty()) {
            // The NEGATIVE bins, because these are USB and the wire order is
            // conjugated -- so the wanted sideband is the lower one, as the
            // assertions at the top of this file establish. Correlating +f here
            // read the SUPPRESSED bin of both captures and compared two
            // leakage figures. It survived on the phasing modulator because its
            // image is proportional to its wanted, so the ratio came out right
            // for the wrong reason; against TXA's image, which is at the
            // numerical floor, that is measuring noise against noise.
            const double out = binPower(iq, -5000.0, kFsOut);
            const double in  = binPower(ref, -kTone, kFsOut);
            // The SAME guard as the sideband ratios, and for the same
            // reason: `out` is a spectral bin, not a level. At 1e-12 against
            // TXA's out of ~2.6e-11 the guard was worth 0.3 dB of the printed
            // rejection -- not dominant the way it was in the headline blocks,
            // but it was the guard and not the chain.
            const double rejDb =
                20.0 * std::log10((in + kSidebandEps) / (out + kSidebandEps));
            double mxOut = 0.0, mxRef = 0.0;
            for (const auto& v : iq)  mxOut = std::max(mxOut, static_cast<double>(std::abs(v)));
            for (const auto& v : ref) mxRef = std::max(mxRef, static_cast<double>(std::abs(v)));
            // Probe where the 5 kHz energy actually went.
            std::fprintf(stderr, "passband: 1 kHz %.6f vs 5 kHz %.6e, rejection %.2f dB%s "
                                 "| max|IQ| ref %.4f out %.4f | out@-5k %.6e out@19k %.6e out@1k %.6e\n",
                         in, out, rejDb, floorMark(rejDb), mxRef, mxOut,
                         binPower(iq, -5000.0, kFsOut), binPower(iq, 19000.0, kFsOut),
                         binPower(iq, 1000.0, kFsOut));
            check(rejDb > 30.0, "audio above the TX passband is filtered out");
        }
    }

    // ---- mic gain scales the drive, and the peak meter follows it ----
    // Runs with the ALC OFF: with it on, compensating for exactly this change is
    // the ALC's purpose, and the 1:1 relationship correctly disappears.
    {
        float peakUnity = -300.0f, peakHalf = -300.0f;
        const auto loud  = modulate(WdspChannel::Mode::Usb, kTone, 0.5, 1.0, 0.5, &peakUnity);
        const auto quiet = modulate(WdspChannel::Mode::Usb, kTone, 0.5, 0.5, 0.5, &peakHalf);
        if (!loud.empty() && !quiet.empty()) {
            // The WANTED bin. See the passband case above for why +kTone was
            // wrong here and why it nevertheless passed.
            const double a = binPower(loud, -kTone, kFsOut);
            const double b = binPower(quiet, -kTone, kFsOut);
            const double dropDb = 20.0 * std::log10((a + 1e-12) / (b + 1e-12));
            std::fprintf(stderr, "mic gain: unity %.6f, half %.6f, drop %.1f dB; "
                                 "peak %.1f -> %.1f dBFS\n",
                         a, b, dropDb, peakUnity, peakHalf);
            check(dropDb > 4.0 && dropDb < 8.0,
                  "halving mic gain drops the transmitted level by ~6 dB");
            check(peakUnity - peakHalf > 4.0 && peakUnity - peakHalf < 8.0,
                  "the mic peak meter follows mic gain by the same ~6 dB");
        }
    }

    // ---- The mic slider lifts speech-level audio to something that modulates -
    //
    // This is the gap that made voice inaudible on the air: measured on the
    // radio, audio at -10 dBFS produced 1226 counts of forward power and audio
    // at -30 dBFS produced 47, while ordinary speech sits near -32 dBFS. That
    // measurement is why the gap must be closed by SOMETHING, and it has not
    // changed.
    //
    // WHAT CLOSES IT HAS. This case used to assert that the ALC lifted a
    // -34 dBFS tone toward full modulation on its own, at unity mic gain — up
    // to 40 dB of makeup, applied on an absolute threshold, which is what lifted
    // room noise level with speech (20.5 dB of separation in, 0.33 dB out) and
    // is the defect this stage's ceiling now forbids. The ALC only reduces.
    // The operator's mic slider closes the gap instead, which is why it reaches
    // +40 dB, so this case is re-pointed at the slider rather than deleted: the
    // same tone, the same destination, a different instrument.
    //
    // Two claims, and they are the pair: at the top of the slider the tone still
    // reaches full modulation, and at unity it now passes straight through
    // instead of being normalized.
    {
        constexpr double kQuiet = 0.02;         // about -34 dBFS, speech-ish
        const double kSlider100Linear = micSliderToLinear(100);
        const auto plain = modulate(WdspChannel::Mode::Usb, kTone, kQuiet, 1.0, 1.5,
                                    nullptr, false);
        const auto alced = modulate(WdspChannel::Mode::Usb, kTone, kQuiet, 1.0, 1.5,
                                    nullptr, true);
        const auto driven = modulate(WdspChannel::Mode::Usb, kTone, kQuiet,
                                     kSlider100Linear, 1.5, nullptr, true);
        if (!plain.empty() && !alced.empty() && !driven.empty()) {
            // Compare the settled tail: the ALC ramps in, so the opening block
            // is deliberately not representative.
            auto tailPeak = [](const std::vector<std::complex<float>>& v) {
                double mx = 0.0;
                for (std::size_t i = v.size() / 2; i < v.size(); ++i)
                    mx = std::max(mx, static_cast<double>(std::abs(v[i])));
                return mx;
            };
            const double a = tailPeak(plain);
            const double b = tailPeak(alced);
            const double c = tailPeak(driven);
            std::fprintf(stderr,
                         "mic slider: quiet input peak %.4f -> ALC at unity %.4f "
                         "(%+.1f dB) -> slider 100 %.4f (%+.1f dB)\n",
                         a, b, 20.0 * std::log10((b + 1e-12) / (a + 1e-12)),
                         c, 20.0 * std::log10((c + 1e-12) / (a + 1e-12)));
            // The ALC adds nothing of its own. Anything but ~0 dB here is the
            // makeup half coming back.
            check(std::fabs(20.0 * std::log10((b + 1e-12) / (a + 1e-12))) < 1.0,
                  "at unity mic gain the ALC passes speech-level audio through "
                  "unchanged");
            // The same bound the old assertion used, now asked of the control
            // that is actually meant to close the gap.
            check(c > 0.5, "the mic slider at 100 brings quiet audio near full "
                           "modulation");
            // And the stage must never overshoot into clipping — an ALC that
            // overshoots transmits splatter rather than merely clipping our own
            // audio. This is measured on the DRIVEN run, which is the only one
            // that now reaches the ALC's target at all, and it is the whole of
            // what the stage still promises.
            double mx = 0.0;
            for (const auto& v : driven) mx = std::max(mx, static_cast<double>(std::abs(v)));
            check(mx <= 1.001, "ALC output never exceeds full scale");
        }
    }

    // ── The mic path preserves the separation between speech and the room ───
    //
    // THE REGRESSION TEST FOR THE REPORTED FAULT, and the direct successor of a
    // case that asserted the opposite. This block used to assert that the ALC
    // HELD its gain below -45 dBFS and lifted above it — makeup gain with an
    // absolute threshold, which sounds like a noise policy and is not one. The
    // threshold sat below a real shack's noise floor, so between words the loop
    // went on lifting until the room reached the same target peak as the voice:
    // measured on the air, 20.5 dB of speech-to-floor separation went in and
    // 0.33 dB came out. A stage that erases 20 dB of contrast is not protecting
    // anything.
    //
    // Rewritten rather than adjusted, because merely relaxing the old numbers
    // would leave "the ALC held" being asserted by a stage that no longer holds
    // anything — the first of its three assertions would still pass, for the
    // wrong reason, measuring "nothing is ever lifted".
    //
    // The property now under test is the one the report is about: the ALC adds
    // no gain at either level, and the DIFFERENCE between them survives the
    // stage intact. 20 dB in, 20 dB out.
    {
        struct HoldRun { double gainDb = 0.0; double outPeak = 0.0;
                         unsigned long long faults = 0; };
        auto settledOnce = [](double amplitude) -> HoldRun {
            HoldRun out;
            Hl2TxDsp tx;
            TxTestAuthority authority;
            Hl2TxDsp::Config cfg;
            cfg.mode = WdspChannel::Mode::Usb;
            cfg.alcEnabled = true;
            std::string err;
            if (!tx.configure(cfg, &err)) {
                std::fprintf(stderr, "FAIL: ALC separation configure: %s\n",
                             err.c_str());
                ++g_failures;
                return out;
            }
            double lastGainDb = 0.0;
            QObject::connect(&tx, &Hl2TxDsp::alcGain, &tx,
                             [&lastGainDb](float db) { lastGainDb = db; });
            // `captured`, not `out`: `out` is the HoldRun this lambda returns.
            std::vector<std::complex<float>> captured;
            QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                             [&captured](const std::vector<std::complex<float>>& iq) {
                captured.insert(captured.end(), iq.begin(), iq.end());
            });

            const int fs = cfg.inputSampleRateHz;
            const int total = fs * 3;   // long enough for the slow release to settle
            constexpr std::size_t kChunk = 240;
            std::vector<float> chunk(kChunk);
            for (int off = 0; off < total; off += static_cast<int>(kChunk)) {
                for (std::size_t n = 0; n < kChunk; ++n) {
                    chunk[n] = static_cast<float>(
                        amplitude * std::sin(2.0 * M_PI * 1000.0
                                             * (off + static_cast<int>(n)) / fs));
                }
                feed(tx, chunk, TxAudioSource::Microphone, authority.context);
            }
            // Settled tail only, so the opening blocks are not representative.
            double mx = 0.0;
            for (std::size_t i = captured.size() / 2; i < captured.size(); ++i)
                mx = std::max(mx, static_cast<double>(std::abs(captured[i])));
            out.gainDb = lastGainDb;
            out.outPeak = mx;
            out.faults = tx.modulatorFaultBlocks();
            return out;
        };
        auto settled = [&](double amplitude) {
            return retryIfStarved<HoldRun>(
                "the ALC hold case",
                [&] { return settledOnce(amplitude); });
        };

        // -54 dBFS: this is "the room" — the fan, the hiss, the pause between
        // words. It sat below the old -45 dBFS hold threshold on purpose, and
        // still sits below it in spirit: it is the leg that used to be hauled up.
        const HoldRun room = settled(0.002);
        // -34 dBFS: about where real speech sits, per the measurements quoted in
        // Hl2TxDsp::Config. Exactly 20 dB above the room leg.
        const HoldRun speech = settled(0.02);

        const double outSeparationDb =
            20.0 * std::log10((speech.outPeak + 1e-12) / (room.outPeak + 1e-12));
        std::fprintf(stderr,
                     "separation: room gain %.2f dB (peak %.6f), speech gain %.2f dB "
                     "(peak %.6f), 20.0 dB in -> %.2f dB out\n",
                     room.gainDb, room.outPeak, speech.gainDb, speech.outPeak,
                     outSeparationDb);
        // Neither leg is lifted. The ceiling is unity, so on any input below the
        // target the ALC's answer is 0 dB — for the room AND for the speech.
        check(std::fabs(room.gainDb) < 1.0,
              "the ALC adds no gain to room-level audio");
        check(std::fabs(speech.gainDb) < 1.0,
              "the ALC adds no gain to speech-level audio either");
        // AND THE ONE THAT WOULD HAVE CAUGHT THE FAULT. Both legs at 0 dB is
        // necessary but not sufficient: what the operator hears is the contrast,
        // and the reported failure was 20.5 dB in arriving as 0.33 dB out.
        check(outSeparationDb > 19.0 && outSeparationDb < 21.0,
              "20 dB of input separation arrives as 20 dB of output separation");

        // AND THE REPORTER'S OWN LEVELS, verbatim. The pair above is a round
        // 20 dB chosen for legibility; #5463's digital-loopback table is
        // -37.05 dBFS in the pauses and -12.4 dBFS on speech, measured at the
        // ALC's own input, and it is the leg where the merge base produced
        // 0.33 dB of output separation from 24.65 dB of input. Asserting the
        // issue's numbers rather than a tidied version of them is what makes
        // this the regression test for the report rather than for the rewrite.
        const HoldRun reported  = settled(0.01405);   // -37.05 dBFS
        const HoldRun reportedSpeech = settled(0.2399);   // -12.4 dBFS
        const double reportedSeparationDb =
            20.0 * std::log10((reportedSpeech.outPeak + 1e-12)
                              / (reported.outPeak + 1e-12));
        std::fprintf(stderr,
                     "separation (#5463's own levels): 24.65 dB in -> %.2f dB out "
                     "(merge base gave 0.33)\n", reportedSeparationDb);
        check(reportedSeparationDb > 23.6 && reportedSeparationDb < 25.7,
              "#5463's own 24.65 dB of input separation survives the stage");
    }

    // ── #4796: client-leveled audio bypasses the ALC entirely ──────────────
    //
    // A TCI/DAX client's level control is a digital attenuator on the audio it
    // streams (WSJT-X's Pwr slider). Through the ALC's makeup half that control
    // was either normalized away (above the then-current -45 dBFS hold
    // threshold) or frozen into a path-dependent gain (below it). Output must
    // simply be proportional to input — and the quiet leg is 5 dB below where
    // that threshold used to sit, which is where the ALC used to freeze. That
    // ceiling is now unity on every path rather than only this one, so these
    // assertions read the same as they did; they are unchanged on purpose.
    {
        // -50 dBFS and -30 dBFS, both with the ALC configured ON, both marked
        // client-leveled. 20 dB apart in, 20 dB apart out.
        const auto quiet = modulate(WdspChannel::Mode::Usb, kTone, 0.00316, 1.0,
                                    1.0, nullptr, true, nullptr,
                                    TxAudioSource::ClientLeveled);
        const auto loud  = modulate(WdspChannel::Mode::Usb, kTone, 0.0316, 1.0,
                                    1.0, nullptr, true, nullptr,
                                    TxAudioSource::ClientLeveled);
        if (!quiet.empty() && !loud.empty()) {
            // The WANTED bin. See the passband case above.
            const double a = binPower(quiet, -kTone, kFsOut);
            const double b = binPower(loud, -kTone, kFsOut);
            const double ratioDb = 20.0 * std::log10((b + 1e-12) / (a + 1e-12));
            std::fprintf(stderr,
                         "client-leveled: -50 dBFS %.6f, -30 dBFS %.6f, ratio %.1f dB\n",
                         a, b, ratioDb);
            check(ratioDb > 18.0 && ratioDb < 22.0,
                  "client-leveled output is proportional to input (no ALC)");
            // And the quiet tone was NOT hauled up toward the ALC target.
            double mx = 0.0;
            for (const auto& v : quiet) mx = std::max(mx, static_cast<double>(std::abs(v)));
            check(mx < 0.05,
                  "a -50 dBFS client tone stays near -50 dBFS on the wire");
        }
    }

    // ── #4796: the reported hysteresis, as an assertion ─────────────────────
    //
    // One continuous transmission, level swept down-up-down across the hold
    // threshold: -50 dBFS -> -30 dBFS -> -50 dBFS. This is the report's "slide
    // the Pwr slider up, RF appears; slide it back, RF stays" — with the ALC in
    // the path, the third segment came out ~26 dB hotter than the first,
    // because the gain wound up during the loud segment was frozen (not
    // reducing, input below hold) instead of released. Client-leveled audio
    // must be path-independent: equal inputs, equal outputs, whatever came
    // between.
    {
        struct HystRun {
            double firstQuiet = 0.0, loud = 0.0, lastQuiet = 0.0;
            unsigned long long faults = 0;
            bool ran = false;
        };
        auto once = [&]() -> HystRun {
        HystRun result;
        Hl2TxDsp tx;
        TxTestAuthority authority;
        Hl2TxDsp::Config cfg;
        cfg.mode = WdspChannel::Mode::Usb;
        cfg.alcEnabled = true;   // configured ON — the bypass is per-block
        std::string err;
        if (!tx.configure(cfg, &err)) {
            std::fprintf(stderr, "FAIL: hysteresis configure: %s\n", err.c_str());
            ++g_failures;
        } else {
            std::vector<std::complex<float>> out;
            QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                             [&](const std::vector<std::complex<float>>& iq) {
                out.insert(out.end(), iq.begin(), iq.end());
            });

            const int fs = cfg.inputSampleRateHz;
            const double levels[] = {0.00316, 0.0316, 0.00316};
            std::size_t marks[4] = {0, 0, 0, 0};
            constexpr std::size_t kChunk = 240;
            std::vector<float> chunk(kChunk);
            int sample = 0;
            for (int stage = 0; stage < 3; ++stage) {
                for (int off = 0; off < fs; off += static_cast<int>(kChunk)) {
                    for (std::size_t n = 0; n < kChunk; ++n, ++sample) {
                        chunk[n] = static_cast<float>(
                            levels[stage]
                            * std::sin(2.0 * M_PI * 1000.0 * sample / fs));
                    }
                    feed(tx, chunk, TxAudioSource::ClientLeveled, authority.context);
                }
                marks[stage + 1] = out.size();
            }

            // Compare the settled tail of each quiet segment (its second half).
            auto tailPeak = [&](std::size_t from, std::size_t to) {
                double mx = 0.0;
                for (std::size_t i = from + (to - from) / 2; i < to; ++i)
                    mx = std::max(mx, static_cast<double>(std::abs(out[i])));
                return mx;
            };
            result.firstQuiet = tailPeak(marks[0], marks[1]);
            result.loud       = tailPeak(marks[1], marks[2]);
            result.lastQuiet  = tailPeak(marks[2], marks[3]);
            result.faults = tx.modulatorFaultBlocks();
            result.ran = true;
        }
        return result;
        };

        const HystRun r = retryIfStarved<HystRun>("the ALC hysteresis case", once);
        if (r.ran) {
            const double reGainDb =
                20.0 * std::log10((r.lastQuiet + 1e-12) / (r.firstQuiet + 1e-12));
            std::fprintf(stderr,
                         "hysteresis: quiet %.6f -> loud %.6f -> quiet %.6f "
                         "(re-gain %.2f dB)\n",
                         r.firstQuiet, r.loud, r.lastQuiet, reGainDb);
            check(std::fabs(reGainDb) < 1.0,
                  "equal client levels produce equal output before and after a "
                  "loud excursion (no ALC hysteresis)");
            check(r.loud > r.firstQuiet * 5.0,
                  "the loud segment is genuinely louder (sweep is real)");
        }
    }

    // ── #4796 review: the bypass is ONE-SIDED — an over-level client is ────
    //    limited, not clipped.
    //
    // Removing the ALC's makeup half is the #4796 fix. Removing its REDUCTION
    // half would leave the modulator's hard clamp as the only thing between a
    // hot client and the band: m_micGain now reaches 100x (+40 dB — the TX gain
    // slider at 100, Hl2TxLevelPolicy.h), and even the 10x used below puts a
    // full-scale client ~17 dB into that clamp. Flat-topping an SSB modulator
    // input is a splatter generator: the one failure mode here that harms other
    // operators rather than the operator who caused it.
    //
    // Distortion is measured as the CREST FACTOR of the analytic magnitude,
    // not as a harmonic bin. For a single tone |IQ| is constant, so peak/mean
    // is 1.0 however the tone is scaled; clipping ripples it. A DFT bin ratio
    // was tried first and rejected: binPower's absolute output is dominated by
    // frequency-dependent cancellation, so it read the same -5.8 dBc on a tone
    // that provably could not be clipping. It is a same-frequency comparator
    // (which is all the proportionality case above asks of it), not a
    // distortion meter. The clean control below stays in the test so the
    // instrument is validated on every run rather than on the day it was
    // written.
    {
        // A raw linear +20 dB, NOT a slider position. This was written as
        // micSliderToLinear(100) back when the slider topped out at +20 dB; the
        // slider now reaches +40 dB (100x) and the name would be a lie. The
        // value is what the case needs — enough to drive a full-scale client
        // well into limiting — so it is kept and renamed rather than moved.
        constexpr double kHotMicGain = 10.0;   // +20 dB, linear
        constexpr double kHarmTone  = 700.0;
        auto crest = [](const std::vector<std::complex<float>>& v) {
            double mx = 0.0, sum = 0.0;
            for (const auto& x : v) {
                const double m = std::abs(x);
                mx = std::max(mx, m);
                sum += m;
            }
            return mx / (sum / static_cast<double>(v.size()) + 1e-12);
        };
        auto settledTail = [](const std::vector<std::complex<float>>& v) {
            return std::vector<std::complex<float>>(
                v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        };

        const auto hot = modulate(WdspChannel::Mode::Usb, kHarmTone, 1.0,
                                  kHotMicGain, 1.5, nullptr, true, nullptr,
                                  TxAudioSource::ClientLeveled);
        // 24 dB below the ALC target at unity mic gain: the limiter cannot
        // engage, so this is what "undistorted" reads on this instrument.
        const auto clean = modulate(WdspChannel::Mode::Usb, kHarmTone, 0.05,
                                    1.0, 1.5, nullptr, true, nullptr,
                                    TxAudioSource::ClientLeveled);
        if (!hot.empty() && !clean.empty()) {
            const auto tail  = settledTail(hot);     // skips the 5 ms attack
            const auto ctail = settledTail(clean);
            double mx = 0.0;
            for (const auto& v : tail)
                mx = std::max(mx, static_cast<double>(std::abs(v)));
            const double hotCrest   = crest(tail);
            const double cleanCrest = crest(ctail);
            std::fprintf(stderr,
                         "over-level client: settled |IQ| peak %.3f, crest %.4f "
                         "(undistorted control %.4f)\n",
                         mx, hotCrest, cleanCrest);
            check(cleanCrest < 1.05,
                  "crest-factor instrument reads ~1.0 on an undistorted tone");
            check(mx < 0.95,
                  "a full-scale client at +20 dB of mic gain is limited below "
                  "the clamp, not flat-topped by it");
            check(mx > 0.5,
                  "the limited transmission is still on the air (case is real)");
            check(hotCrest < 1.05,
                  "an over-level client is limited cleanly, with no clipping "
                  "distortion on the wire");
        }

        // AND THE TOP OF THE SLIDER, measured over the WHOLE RUN.
        //
        // The case above is a settled-state measurement by construction:
        // settledTail() drops the first half, which is the entire key-on
        // window. That was sound while the slider stopped at +20 dB — at 10x a
        // full-scale source peaks inside the clamp even on the first block, so
        // there was nothing in the excluded half to see. The widening to +40 dB
        // ended that: reset() leaves the ALC at unity and the loop has to come
        // DOWN 40 dB, which on a 5 ms attack and a 5 ms block takes ~17 ms —
        // seventeen milliseconds of flat-topped modulator input at the start of
        // EVERY over, on every path, reported by no meter because TX:ALC is
        // measured after the clamp.
        //
        // So this leg asserts on the whole run rather than the tail, and it is
        // the key-on seed in processAudioBlock that makes it pass. Reverting
        // that seed to the old `m_alcGain += a * (target - m_alcGain)` fails
        // this check and no other in the file, which is what makes it a guard
        // rather than a restatement.
        const double kSliderTopGain = micSliderToLinear(100);   // 100x, +40 dB
        const auto slam = modulate(WdspChannel::Mode::Usb, kHarmTone, 1.0,
                                   kSliderTopGain, 1.5, nullptr, true, nullptr,
                                   TxAudioSource::ClientLeveled);
        if (!slam.empty()) {
            double mx = 0.0;
            std::size_t atClamp = 0;
            for (const auto& v : slam) {
                const double m = std::abs(v);
                mx = std::max(mx, m);
                if (m >= 0.999)
                    ++atClamp;
            }
            // Derived from the run itself (1.5 s above) rather than from a
            // named rate, so the figure stays honest if the modulator's
            // upsample factor ever changes underneath it.
            const double msAtClamp = 1500.0 * static_cast<double>(atClamp)
                                   / static_cast<double>(slam.size());
            std::fprintf(stderr,
                         "slider top: %.0fx full-scale, whole-run |IQ| peak "
                         "%.4f, %zu samples at the clamp (%.1f ms)\n",
                         kSliderTopGain, mx, atClamp, msAtClamp);
            check(mx < 0.999,
                  "the slider at 100 on a full-scale source never reaches the "
                  "modulator's clamp, key-on window included");
            std::fprintf(stderr, "slider top: whole-run crest %.4f, "
                                 "settled-tail crest %.4f\n",
                         crest(slam), crest(settledTail(slam)));
            // THE WINDOW DIFFERS BY BUILD, and the reason is not a tolerance.
            //
            // Crest here is peak over mean |IQ|, so anything that makes the
            // envelope non-constant raises it. In the phasing build the only
            // such thing IS the fault this leg guards -- a key-on window of
            // flat-topped modulator input -- so the window is the whole run,
            // deliberately, and that is what makes this a guard rather than a
            // restatement of the settled case.
            //
            // A TXA channel has a second, LEGITIMATE source of envelope:
            // create_slews' mute ramp, 840 input samples of deliberate T/R
            // shaping at the head of every over. Measured here: whole run
            // 1.0684, settled tail 1.0000 -- with ZERO samples at the clamp
            // over the whole run, which the check above asserts separately.
            // So a whole-run crest test in the TXA build reports the
            // transmitter's intended envelope as distortion, and would fail on
            // a modulator that is in fact cleaner than the one it replaces.
            //
            // The key-on fault cannot occur in the TXA build for the same
            // reason: the ramp starts the modulator's input at zero, so there
            // is no full-scale block before the ALC has converged. The
            // clamp-count assertion above is what pins that, and it runs on the
            // whole run in BOTH builds.
#if AETHER_HL2_TX_TXA
            check(crest(settledTail(slam)) < 1.05,
                  "and puts no clipping distortion on the wire once the T/R "
                  "mute ramp has run");
#else
            check(crest(slam) < 1.05,
                  "and puts no clipping distortion on the wire at any point in "
                  "the over");
#endif
            check(mx > 0.5,
                  "the transmission is still on the air (case is real)");
        }

        // ── AND THE SAME OVER WITH A LEAD-IN, which is the shape a real one
        //    has. The leg above opens at full scale, so the seed's first
        //    reduction IS its first block — the one opening that a seed keyed
        //    on "the first block carrying signal" also handled. Every other
        //    over starts quieter than the ALC target: a microphone's room
        //    floor, a TCI client's ramp-in, a beacon's first symbol.
        //
        //    blockPeak is measured after m_micGain, so at 100x the loop's
        //    `> 1e-6` signal test corresponds to an input of 1e-8 (-160 dBFS)
        //    — it is "not bit-exactly zero", not "carries signal". A seed that
        //    disarms there is spent by the lead-in and the loud block that
        //    follows gets the full 40 dB ramp anyway: measured at |IQ| 1.5391
        //    with 239 samples clipped, the same figure as no seed at all.
        //
        //    So the discriminator is the LEAD-IN, not the level. This leg
        //    fails on a seed gated by signal presence and passes on one gated
        //    by `reducing`, and neither the leg above nor any other case in
        //    this file can tell those two apart.
        {
            const int fs = 24000;
            std::mt19937 rng(20260915);
            std::normal_distribution<double> room(0.0, 0.001 / 3.0);  // -60 dBFS
            std::vector<float> audio;
            for (int n = 0; n < fs / 10; ++n)          // 100 ms of shack
                audio.push_back(static_cast<float>(room(rng)));
            for (int n = 0; n < fs * 3 / 2; ++n)       // then 1.5 s at full scale
                audio.push_back(static_cast<float>(
                    std::sin(2.0 * M_PI * kHarmTone * n / fs) + room(rng)));

            Hl2TxDsp tx;
            Hl2TxDsp::Config cfg;
            cfg.mode = WdspChannel::Mode::Usb;
            cfg.alcEnabled = true;
            std::string err;
            if (!tx.configure(cfg, &err)) {
                std::fprintf(stderr, "FAIL: lead-in configure: %s\n", err.c_str());
                ++g_failures;
            } else {
                tx.reset();                  // exactly what unkey does
                tx.setMicGain(kSliderTopGain);
                std::vector<std::complex<float>> out;
                QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                                 [&out](const std::vector<std::complex<float>>& iq) {
                    out.insert(out.end(), iq.begin(), iq.end());
                });
                constexpr std::size_t kChunk = 240;
                for (std::size_t off = 0; off < audio.size(); off += kChunk) {
                    const std::size_t n = std::min(kChunk, audio.size() - off);
                    // PACED, like every other feed in this file. These three
                    // clipping cases arrived on main after this branch forked,
                    // so they were the only sites still calling the modulator
                    // directly. In the TXA build that starves it -- 64 of 66
                    // blocks underran and the whole-run IQ peak read 0.0000,
                    // which the case then reported as "the transmission is not
                    // on the air". It was on the air; the test was feeding five
                    // times faster than the channel drains. feed() is a no-op
                    // in the phasing build.
                    feed(tx,
                         std::vector<float>(
                             audio.begin() + static_cast<std::ptrdiff_t>(off),
                             audio.begin() + static_cast<std::ptrdiff_t>(off + n)),
                         TxAudioSource::Microphone, authority.context);
                }
                double mx = 0.0;
                std::size_t atClamp = 0;
                for (const auto& v : out) {
                    const double m = std::abs(v);
                    mx = std::max(mx, m);
                    if (m >= 0.999)
                        ++atClamp;
                }
                std::fprintf(stderr,
                             "lead-in: 100 ms of -60 dBFS room then full scale at "
                             "%.0fx, whole-run |IQ| peak %.4f, %zu at the clamp\n",
                             kSliderTopGain, mx, atClamp);
                check(mx < 0.999,
                      "a quiet lead-in does not spend the ALC seed — the loud "
                      "block after it is still caught before the clamp");
                check(atClamp == 0,
                      "and no sample of that over reaches the modulator's clamp");
            }
        }

        // ── THE OPERATOR RAISES THE GAIN MID-OVER, which is the other way the
        //    loop ends up with history that describes a different chain. The
        //    widening doubled how far a single move can jump, and the ALC
        //    cannot attack 40 dB inside one block: before setMicGain() re-armed
        //    the seed, a 50 -> 100 move on a full-scale source flat-topped the
        //    modulator for 17.1 ms (|IQ| 1.4952, 816 samples at the clamp).
        //
        //    The re-arm is upward-only and the seed itself only ever reduces,
        //    so this cannot step an operator's level UP mid-word. That half is
        //    what the second assertion pins.
        {
            const int fs = 24000;
            std::vector<float> audio(static_cast<std::size_t>(fs * 3 / 2));
            for (std::size_t n = 0; n < audio.size(); ++n)
                audio[n] = static_cast<float>(
                    std::sin(2.0 * M_PI * kHarmTone * static_cast<double>(n) / fs));

            Hl2TxDsp tx;
            Hl2TxDsp::Config cfg;
            cfg.mode = WdspChannel::Mode::Usb;
            cfg.alcEnabled = true;
            std::string err;
            if (!tx.configure(cfg, &err)) {
                std::fprintf(stderr, "FAIL: mid-over configure: %s\n", err.c_str());
                ++g_failures;
            } else {
                tx.reset();
                tx.setMicGain(micSliderToLinear(50));     // unity, where an over starts
                std::vector<std::complex<float>> out;
                QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                                 [&out](const std::vector<std::complex<float>>& iq) {
                    out.insert(out.end(), iq.begin(), iq.end());
                });
                constexpr std::size_t kChunk = 240;
                const std::size_t moveAt = audio.size() / 2;
                bool moved = false;
                for (std::size_t off = 0; off < audio.size(); off += kChunk) {
                    if (!moved && off >= moveAt) {
                        tx.setMicGain(kSliderTopGain);    // 50 -> 100, mid-word
                        moved = true;
                    }
                    const std::size_t n = std::min(kChunk, audio.size() - off);
                    // PACED, like every other feed in this file. These three
                    // clipping cases arrived on main after this branch forked,
                    // so they were the only sites still calling the modulator
                    // directly. In the TXA build that starves it -- 64 of 66
                    // blocks underran and the whole-run IQ peak read 0.0000,
                    // which the case then reported as "the transmission is not
                    // on the air". It was on the air; the test was feeding five
                    // times faster than the channel drains. feed() is a no-op
                    // in the phasing build.
                    feed(tx,
                         std::vector<float>(
                             audio.begin() + static_cast<std::ptrdiff_t>(off),
                             audio.begin() + static_cast<std::ptrdiff_t>(off + n)),
                         TxAudioSource::Microphone, authority.context);
                }
                double mx = 0.0;
                std::size_t atClamp = 0;
                for (const auto& v : out) {
                    const double m = std::abs(v);
                    mx = std::max(mx, m);
                    if (m >= 0.999)
                        ++atClamp;
                }
                std::fprintf(stderr,
                             "mid-over: slider 50 -> 100 on a full-scale source, "
                             "whole-run |IQ| peak %.4f, %zu at the clamp\n",
                             mx, atClamp);
                check(mx < 0.999,
                      "raising the mic slider mid-over does not drive the "
                      "modulator into its clamp");
                check(mx > 0.5,
                      "and the transmission is still on the air (case is real)");
            }
        }

        // ── A QUIET WORD, THEN A LOUD ONE, which is what speech is and what
        //    a one-shot key-on seed cannot survive.
        //
        //    An earlier revision protected the opening of the over only: the
        //    first block needing ANY reduction took the jump, and every block
        //    after it got the smoothed attack. A source that crosses the ALC
        //    target gently spends that on a fraction of a dB — and at a 512
        //    sample block on 24 kHz the smoothed attack left 1.4% of whatever
        //    step came next above the modulator's clamp. Measured on this
        //    class at slider 100: a -38 dBFS word then a -12 dBFS syllable
        //    reached |IQ| 1.0768 with 223 samples clipped, and a quiet passage
        //    with one loud burst in it reached 1.5045 with 727 (15.2 ms).
        //
        //    Reduction is instantaneous now, so the shape does not matter,
        //    and this is the case that says so — the three legs above cannot,
        //    because each of them steps only once.
        //
        //    WHAT THIS PINS IS THE OBSERVABLE PROPERTY, not the mechanism: a
        //    smoothed attack fast enough to close the step inside one block
        //    passes it too (0.5 ms does; 5 ms does not). That is the honest
        //    reading and it is also the argument for instantaneous — whether a
        //    given time constant is "fast enough" is a function of dspBlockSize
        //    and inputSampleRateHz, so it is a guarantee that quietly expires
        //    the day either changes. Instantaneous has no such dependency.
        {
            const int fs = 24000;
            const double quiet = std::pow(10.0, -38.0 / 20.0);   // just over target
            const double loud  = std::pow(10.0, -12.0 / 20.0);   // an ordinary syllable
            std::vector<float> audio;
            auto push = [&](double amp, double seconds) {
                const int n0 = static_cast<int>(audio.size());
                for (int n = 0; n < static_cast<int>(seconds * fs); ++n)
                    audio.push_back(static_cast<float>(
                        amp * std::sin(2.0 * M_PI * kHarmTone * (n0 + n) / fs)));
            };
            push(quiet, 0.40);        // the quiet word spends a one-shot seed
            push(loud,  0.60);        // the loud one arrives with no protection
            push(quiet, 0.20);        // and it must release again afterwards

            Hl2TxDsp tx;
            Hl2TxDsp::Config cfg;
            cfg.mode = WdspChannel::Mode::Usb;
            cfg.alcEnabled = true;
            std::string err;
            if (!tx.configure(cfg, &err)) {
                std::fprintf(stderr, "FAIL: crescendo configure: %s\n", err.c_str());
                ++g_failures;
            } else {
                tx.reset();
                tx.setMicGain(kSliderTopGain);
                std::vector<std::complex<float>> out;
                QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                                 [&out](const std::vector<std::complex<float>>& iq) {
                    out.insert(out.end(), iq.begin(), iq.end());
                });
                constexpr std::size_t kChunk = 240;
                for (std::size_t off = 0; off < audio.size(); off += kChunk) {
                    const std::size_t n = std::min(kChunk, audio.size() - off);
                    // PACED, like every other feed in this file. These three
                    // clipping cases arrived on main after this branch forked,
                    // so they were the only sites still calling the modulator
                    // directly. In the TXA build that starves it -- 64 of 66
                    // blocks underran and the whole-run IQ peak read 0.0000,
                    // which the case then reported as "the transmission is not
                    // on the air". It was on the air; the test was feeding five
                    // times faster than the channel drains. feed() is a no-op
                    // in the phasing build.
                    feed(tx,
                         std::vector<float>(
                             audio.begin() + static_cast<std::ptrdiff_t>(off),
                             audio.begin() + static_cast<std::ptrdiff_t>(off + n)),
                         TxAudioSource::Microphone, authority.context);
                }
                double mx = 0.0;
                std::size_t atClamp = 0;
                for (const auto& v : out) {
                    const double m = std::abs(v);
                    mx = std::max(mx, m);
                    if (m >= 0.999)
                        ++atClamp;
                }
                std::fprintf(stderr,
                             "quiet-then-loud: -38 dBFS word then -12 dBFS syllable "
                             "at %.0fx, whole-run |IQ| peak %.4f, %zu at the clamp\n",
                             kSliderTopGain, mx, atClamp);
                check(mx < 0.999,
                      "a loud syllable after a quiet one is caught before the "
                      "clamp — reduction does not depend on where in the over "
                      "the step arrives");
                check(atClamp == 0,
                      "and no sample of that over reaches the clamp");
                check(mx > 0.5,
                      "and the transmission is still on the air (case is real)");
            }
        }
    }

    // ── #4796 review: the reduction half must RELEASE — it may not latch ────
    //
    // Gating only the ALC's INCREASE branch (leaving `reducing` free to act) is
    // the smaller edit and is wrong: once a loud block pulls the gain down,
    // nothing can raise it again within the over, so a client sliding back down
    // stays attenuated at whatever the loudest block called for. That is
    // path-dependent gain — #4796's own defect class, mirrored. Expressing the
    // bypass as a unity CEILING instead leaves the gain free to release.
    //
    // This case is the discriminator between those two shapes. It passes on a
    // total bypass and on the ceiling; it fails ~21 dB wide on an
    // increase-gated ALC.
    //
    // HALF OF THAT RATIONALE HAS RETIRED, and the half that has not is the
    // reason this case is untouched by the unity-ceiling change.
    //
    // It used to be the discriminator for a second thing as well: the hold being
    // made mic-path-only (`!clientLeveled` in processAudioBlock's `held`). There
    // is no hold any more — it was deleted outright with the ALC's makeup half,
    // because under a unity ceiling a quiet block wants target = 1.0, so after
    // any reduction `reducing` is false and a hold would strand the gain exactly
    // as described above. So that discriminator has nothing left to discriminate
    // between, and the paragraph is kept as history rather than as a live claim.
    //
    // WHAT SURVIVES IS THE PROOF THAT THE UNITY CEILING CHANGED NOTHING HERE. On
    // the client path the ceiling was already 1.0 and `held` was already false,
    // so every assertion below reads the same before and after — which is what
    // makes this case the evidence that a change to the mic path is a no-op on
    // TCI/DAX.
    //
    // THE QUIET LEG IS STILL DELIBERATELY WHERE IT IS: -70 dBFS times this
    // case's 10x mic gain is -50 dBFS at the ALC's measurement point, which was
    // under the old -45 dBFS hold threshold. Do not raise it. It is what keeps
    // the case able to catch a hold reappearing, on either path, by any route.
    //
    // One continuous transmission: full scale, then -70 dBFS held for four
    // release time constants. The tail must match the same level keyed fresh.
    {
        // A raw linear +20 dB, not a slider position — see the note in the case
        // above. The slider reaches +40 dB now; this value does not need to.
        constexpr double kHotMicGain = 10.0;
        const auto fresh = modulate(WdspChannel::Mode::Usb, kTone, 0.000316,
                                    kHotMicGain, 1.5, nullptr, true, nullptr,
                                    TxAudioSource::ClientLeveled);

        struct RelRun {
            double swept = 0.0, lastGainDb = 0.0;
            unsigned long long faults = 0;
            bool ran = false;
        };
        auto once = [&]() -> RelRun {
        RelRun result;
        Hl2TxDsp tx;
        TxTestAuthority authority;
        Hl2TxDsp::Config cfg;
        cfg.mode = WdspChannel::Mode::Usb;
        cfg.alcEnabled = true;
        std::string err;
        if (!tx.configure(cfg, &err)) {
            std::fprintf(stderr, "FAIL: limiter release configure: %s\n",
                         err.c_str());
            ++g_failures;
        } else if (!fresh.empty()) {
            tx.setMicGain(kHotMicGain);
            std::vector<std::complex<float>> out;
            QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                             [&](const std::vector<std::complex<float>>& iq) {
                out.insert(out.end(), iq.begin(), iq.end());
            });
            double lastGainDb = 0.0;
            QObject::connect(&tx, &Hl2TxDsp::alcGain, &tx,
                             [&lastGainDb](float db) { lastGainDb = db; });

            const int fs = cfg.inputSampleRateHz;
            constexpr std::size_t kChunk = 240;
            std::vector<float> chunk(kChunk);
            const double levels[]  = {1.0, 0.000316};
            const int    stageSec[] = {1, 2};
            int sample = 0;
            std::size_t afterLoud = 0;
            for (int stage = 0; stage < 2; ++stage) {
                for (int off = 0; off < fs * stageSec[stage];
                     off += static_cast<int>(kChunk)) {
                    for (std::size_t n = 0; n < kChunk; ++n, ++sample) {
                        chunk[n] = static_cast<float>(
                            levels[stage]
                            * std::sin(2.0 * M_PI * 1000.0 * sample / fs));
                    }
                    feed(tx, chunk, TxAudioSource::ClientLeveled, authority.context);
                }
                if (stage == 0)
                    afterLoud = out.size();
            }

            double swept = 0.0;
            for (std::size_t i = afterLoud + (out.size() - afterLoud) / 2;
                 i < out.size(); ++i)
                swept = std::max(swept, static_cast<double>(std::abs(out[i])));
            result.swept = swept;
            result.lastGainDb = lastGainDb;
            result.faults = tx.modulatorFaultBlocks();
            result.ran = true;
        }
        return result;
        };

        const RelRun r = retryIfStarved<RelRun>("the limiter release case", once);
        if (r.ran) {
            double ref = 0.0;
            for (std::size_t i = fresh.size() / 2; i < fresh.size(); ++i)
                ref = std::max(ref, static_cast<double>(std::abs(fresh[i])));
            const double deltaDb =
                20.0 * std::log10((r.swept + 1e-12) / (ref + 1e-12));
            std::fprintf(stderr,
                         "limiter release: swept %.6f vs fresh-key %.6f "
                         "(delta %.2f dB, settled ALC %.2f dB)\n",
                         r.swept, ref, deltaDb, r.lastGainDb);
            check(std::fabs(deltaDb) < 1.0,
                  "the limiter releases to unity after an over-level excursion "
                  "(reduction does not latch)");
            check(std::fabs(r.lastGainDb) < 1.0,
                  "ALC gain is back at 0 dB once the client is quiet again");
        }
    }

    // ── The engine's own generated audio keeps the level it was generated at ──
    //
    // The WSPR pump reaches the modulator as TxAudioSource::EngineGenerated —
    // the only source tagged so. Two properties, and the second is the one that
    // was actually broken on the air.
    //
    // 1. THE MIC SLIDER DOES NOT REACH IT. A slider set for a voice is not a
    //    control over an unattended beacon. Before the source enum, engine
    //    audio was indistinguishable from mic audio and moved with it.
    // 2. THE LEVEL SURVIVES. A generator emitting -20 dBFS transmits -20 dBFS.
    //
    // Both were invisible while the ALC carried 40 dB of makeup, because it
    // normalised every generated level onto its target. The bench measured the
    // consequence once the makeup went: 18.58 dB of unattended shortfall
    // (bench-runner runs/wspr-unattended-ab, both binaries, same stimulus).
    {
        constexpr double kGenerated = 0.1;      // -20.000 dBFS, the WSPR default
        // A mic peak per call, not one scratch variable reused and discarded:
        // on the EngineGenerated path processAudioBlock computes preAlc from the
        // SUBSTITUTED multiplier, so this is the pre-slider level, and it is the
        // one number that separates "the slider was bypassed" from "the slider
        // happened to sit at unity". Asserted below.
        float mpUnity = 0.0f, mpUp = 0.0f, mpDown = 0.0f, mpMic = 0.0f;
        auto peak = [](const std::vector<std::complex<float>>& iq) {
            double m = 0.0;
            for (const auto& v : iq) m = std::max(m, static_cast<double>(std::abs(v)));
            return m;
        };

        const double pUnity = peak(modulate(WdspChannel::Mode::Usb, kTone,
            kGenerated, 1.0, 1.0, &mpUnity, true, nullptr, TxAudioSource::EngineGenerated));
        const double pUp    = peak(modulate(WdspChannel::Mode::Usb, kTone,
            kGenerated, 100.0, 1.0, &mpUp, true, nullptr, TxAudioSource::EngineGenerated));
        const double pDown  = peak(modulate(WdspChannel::Mode::Usb, kTone,
            kGenerated, 0.1, 1.0, &mpDown, true, nullptr, TxAudioSource::EngineGenerated));

        const double spreadDb = 20.0 * std::log10(
            std::max(pUp, pDown) / std::max(1e-12, std::min(pUp, pDown)));
        std::fprintf(stderr,
            "engine-generated: mic 1x %.6f, 100x %.6f, 0.1x %.6f -> spread %.2f dB\n",
            pUnity, pUp, pDown, spreadDb);
        check(spreadDb < 0.5,
              "the mic slider does not reach engine-generated audio");

        // THE METER AGREES WITH THE AIR. The IQ spread above could in principle
        // be flat because something downstream re-levelled it; the pre-ALC peak
        // is measured at the multiplier itself, so a slider that still reached
        // this audio would move THIS by 1000x whatever the modulator did after.
        // 100x, 1x and 0.1x must all report the generated level.
        // micPeak() publishes dBFS, so the spread is a difference, not a ratio.
        const double mpSpreadDb = std::max({mpUnity, mpUp, mpDown})
                                - std::min({mpUnity, mpUp, mpDown});
        const double generatedDbfs = 20.0 * std::log10(kGenerated);
        std::fprintf(stderr,
            "engine-generated pre-ALC mic peak: 1x %.3f, 100x %.3f, 0.1x %.3f dBFS"
            " -> spread %.2f dB (generated %.3f dBFS)\n",
            mpUnity, mpUp, mpDown, mpSpreadDb, generatedDbfs);
        check(mpSpreadDb < 0.1,
              "the pre-ALC mic peak is unmoved by the slider for engine audio");
        check(std::fabs(static_cast<double>(mpUnity) - generatedDbfs) < 0.1,
              "the pre-ALC mic peak reports the level the generator chose");

        // And the mic path is untouched by all of it. This one is also the
        // CONTROL for the level assertion below.
        const double micUnity = peak(modulate(WdspChannel::Mode::Usb, kTone,
            kGenerated, 1.0, 1.0, &mpMic, true));

        // Consistency alone is not enough -- three identical WRONG answers
        // would pass the spread check. The level must be RIGHT, and "right" is
        // measured rather than asserted against a constant: the modulator has
        // its own scale factor (an amplitude of 0.5 leaves as |IQ| 0.52, see
        // the diag line above), so dividing by a guessed number would test the
        // guess. The microphone path at UNITY mic gain applies no gain either,
        // so it IS the reference -- and the claim becomes exactly what it
        // should be: engine-generated audio comes out where mic audio would
        // with the slider at unity, whatever the slider actually says.
        const double vsControlDb =
            20.0 * std::log10(std::max(1e-12, pUnity / micUnity));
        std::fprintf(stderr,
            "engine-generated vs mic-at-unity control: %+.3f dB\n", vsControlDb);
        check(std::fabs(vsControlDb) < 0.1,
              "engine-generated audio transmits at the level it was generated at");
        const double micHalf  = peak(modulate(WdspChannel::Mode::Usb, kTone,
            kGenerated, 0.5, 1.0, &mpMic, true));
        const double micDropDb =
            20.0 * std::log10(std::max(1e-12, micHalf / micUnity));
        std::fprintf(stderr,
            "mic path still follows the slider: %.2f dB for a 2:1 cut\n", micDropDb);
        check(micDropDb < -3.0, "the mic slider still moves microphone audio");
    }

    // ── Residue from one source is never levelled as another ─────────────
    //
    // m_inBuffer carries up to dspBlockSize-1 samples between calls, and the
    // multiplier is chosen from the CURRENT block's source and applied to all of
    // them. While every source shared one multiplier that cost nothing. Now
    // EngineGenerated bypasses m_micGain, so a carried sample can be levelled up
    // to 40 dB from where its own source wanted it.
    //
    // Nothing upstream should interleave two sources inside one transmission
    // (startWsprPump's setDaxTxMode(true) fences the mic path, feedDaxTxAudio's
    // m_wsprBeacon->isActive() fences the client path), but that is an argument
    // about call sites in another class. This is the structural half.
    {
        Hl2TxDsp tx;
        Hl2TxDsp::Config cfg;
        cfg.mode = WdspChannel::Mode::Usb;
        cfg.alcEnabled = true;
        std::string err;
        if (!tx.configure(cfg, &err)) {
            std::fprintf(stderr, "FAIL: residue configure: %s\n", err.c_str());
            ++g_failures;
        } else {
            tx.setMicGain(100.0);        // +40 dB, the top of the slider
            float micPeakDb = -999.0f;
            QObject::connect(&tx, &Hl2TxDsp::micPeak, &tx,
                             [&micPeakDb](float db) { micPeakDb = db; });
            std::vector<std::complex<float>> out;
            QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                             [&out](const std::vector<std::complex<float>>& iq) {
                out.insert(out.end(), iq.begin(), iq.end());
            });

            // Half a block of engine-generated tone: buffered, nothing emitted.
            const std::size_t half = static_cast<std::size_t>(cfg.dspBlockSize) / 2;
            std::vector<float> tone(half);
            for (std::size_t n = 0; n < half; ++n) {
                tone[n] = static_cast<float>(
                    0.1 * std::sin(2.0 * M_PI * 1000.0 * static_cast<double>(n)
                                   / cfg.inputSampleRateHz));
            }
            tx.processAudioBlock(tone, TxAudioSource::EngineGenerated, authority.context);
            check(out.empty() && micPeakDb < -998.0f,
                  "a partial block emits nothing and is carried");

            // Now half a block of SILENCE tagged Microphone. The carried engine
            // samples would complete the block and be multiplied by the mic
            // slider's 100x -- a -20 dBFS beacon arriving on the air at full
            // scale. With the guard they are dropped, the buffer holds only
            // silence, and it is under a block again: nothing is emitted.
            const std::vector<float> silence(half, 0.0f);
            tx.processAudioBlock(silence, TxAudioSource::Microphone, authority.context);
            std::fprintf(stderr,
                "residue guard: after a source change, emitted %zu IQ samples,"
                " mic peak %.1f dBFS\n", out.size(), micPeakDb);
            check(out.empty(),
                  "engine residue is dropped, not levelled as the new source");
            check(micPeakDb < -998.0f,
                  "no block is completed out of two sources' samples");
        }
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_txdsp_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
