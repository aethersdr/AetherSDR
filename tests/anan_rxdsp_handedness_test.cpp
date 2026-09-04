// aetherd ANAN P2 Phase 1b -- AnanRxDsp handedness + DC-blocker test.
//
// *** READ HERMES.md §16 ("Receive handedness and tuning — the two-error
// trap") BEFORE CHANGING ANY EXPECTED VALUE IN THIS FILE. *** It documents
// the most expensive bug in this project's history: Hl2RxDsp handed the
// demodulator and the spectrum the wrong IQ handedness for a full bring-up
// cycle, and the unit tests of the day passed throughout, because (per
// HERMES §16.5) they fed IQ in the TEXTBOOK convention rather than the
// wire's own -- exp(-jwt) instead of exp(+jwt) -- so a mirrored spectrum and
// an inverted demodulator both looked correct. This file generates every
// tone in WIRE convention for exactly that reason.
//
// This file proves two things:
//
//   1. (Confident, no radio involved.) AnanSpectrum's FFT+fftshift correctly
//      mirrors a conjugated tone to the opposite side of DC. This is the
//      mathematical building block the whole conjugate-split design depends
//      on, and it is provable with no assumption about which way the real
//      wire's handedness goes.
//   2. (CONFIRMED, 2026-08-21 -- was provisional, now pins a bench-verified
//      fact, not just current code behaviour.) AnanRxDsp's conjugate split
//      (demodulator raw, spectrum conjugated -- the same split Hl2RxDsp
//      settled on for Protocol 1) is the actually-correct one for a real
//      ANAN-G2: `radiocert rx` (real WWV carrier) showed the textbook
//      USB/DIGU-recover, LSB/DIGL-don't signature, and an independent
//      RSP1B/SDR++ receiver reproduced the identical pattern at the same
//      dial/offset geometry -- the two-source bar this comment used to say
//      was still outstanding. A synthetic tone at a known wire-convention
//      offset demodulating to the expected audio frequency in USB and LSB,
//      below, now pins a PROVEN fact.
//
// Also includes an AM DC-pedestal regression check mirroring
// hl2_am_dcblock_test's ratio-based methodology: WDSP's envelope detector is
// a WDSP fact, not a Hermes-Lite fact, and this class reuses the identical
// DcBlocker code, so it is tested with full confidence, no hedging.

#include "core/backends/anan/AnanRxDsp.h"
#include "core/backends/anan/AnanSpectrum.h"

#include <QCoreApplication>

#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace AetherSDR::anan;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---- Group 1 helpers: AnanSpectrum peak-bin location ----

int peakBin(const std::vector<float>& binsDbfs)
{
    int best = 0;
    float bestVal = binsDbfs.empty() ? 0.0f : binsDbfs[0];
    for (std::size_t i = 1; i < binsDbfs.size(); ++i) {
        if (binsDbfs[i] > bestVal) { bestVal = binsDbfs[i]; best = static_cast<int>(i); }
    }
    return best;
}

// A pure tone at exactly `cyclesPerFrame` cycles over `n` samples -- an
// integer bin frequency, so the FFT peak lands on one bin with no leakage
// ambiguity from the Hanning window.
std::vector<std::complex<float>> makeTone(int n, double cyclesPerFrame, bool conjugate)
{
    std::vector<std::complex<float>> out(static_cast<std::size_t>(n));
    const double dp = 2.0 * kPi * cyclesPerFrame / n;
    for (int k = 0; k < n; ++k) {
        const double phase = dp * k;
        const float re = static_cast<float>(std::cos(phase));
        const float im = static_cast<float>(std::sin(phase));
        out[static_cast<std::size_t>(k)] = conjugate ? std::complex<float>(re, -im)
                                                     : std::complex<float>(re, im);
    }
    return out;
}

// ---- Group 2 helpers: end-to-end demodulated-audio frequency ----
// Mirrors hl2_shift_test.cpp's runTone/dominantHz exactly.

double dominantHz(const std::vector<float>& mono, int rate)
{
    const std::size_t n = mono.size();
    if (n < 64) return -1.0;
    double bestF = -1.0, bestMag = -1.0;
    for (double f = 100.0; f <= rate / 2.0 - 100.0; f += 25.0) {
        double re = 0.0, im = 0.0;
        const double w = 2.0 * kPi * f / rate;
        for (std::size_t k = 0; k < n; ++k) {
            re += mono[k] * std::cos(w * static_cast<double>(k));
            im += mono[k] * std::sin(w * static_cast<double>(k));
        }
        const double mag = re * re + im * im;
        if (mag > bestMag) { bestMag = mag; bestF = f; }
    }
    return bestF;
}

constexpr int kInputRate = 48000;
constexpr int kAudioRate = 48000;   // keep audio == input so Hz map 1:1
constexpr int kBlock = 1024;

// Feed a WIRE-convention tone and return the dominant recovered audio
// frequency. `wireOffsetHz` is in wire sign, not analytic sign -- per
// HERMES §16.5, a POSITIVE value here is "wire order", and since demod =
// raw wire is the bench-confirmed correct split (see the file header),
// that is the same sign a caller would reason about in textbook terms
// too, since facts 1 and 2 cancel for the demodulator.
double runTone(AnanRxDsp& dsp, double wireOffsetHz, double shiftHz)
{
    dsp.setShift(shiftHz);

    std::vector<float> audio;
    const auto conn = QObject::connect(&dsp, &AnanRxDsp::audioReady,
                     [&audio](const std::vector<float>& stereo) {
        for (std::size_t k = 0; k + 1 < stereo.size(); k += 2)
            audio.push_back(stereo[k]);            // left channel
    });

    constexpr int kWarmBlocks = 24;
    constexpr int kMeasureBlocks = 24;
    double phase = 0.0;
    const double dp = 2.0 * kPi * wireOffsetHz / kInputRate;
    for (int b = 0; b < kWarmBlocks + kMeasureBlocks; ++b) {
        std::vector<std::complex<float>> iq(kBlock);
        for (int k = 0; k < kBlock; ++k) {
            iq[static_cast<std::size_t>(k)] = {
                static_cast<float>(0.25 * std::cos(phase)),
                static_cast<float>(0.25 * std::sin(phase))
            };
            phase += dp;
            if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
        }
        if (b == kWarmBlocks) audio.clear();
        dsp.processIqBlock(iq);
    }
    QObject::disconnect(conn);
    return dominantHz(audio, kAudioRate);
}

// ---- Group 3 helpers: AM DC pedestal ----
// Mirrors hl2_am_dcblock_test.cpp's runAm/AudioStats.

struct AudioStats {
    double mean = 0.0;
    double acRms = 0.0;
};

AudioStats runAm(AnanRxDsp& dsp, int inputRateHz, double seconds, double tailSeconds,
                 double carrierOffsetHz, double modHz, double modDepth)
{
    std::vector<float> audio;
    const auto conn = QObject::connect(&dsp, &AnanRxDsp::audioReady, &dsp,
        [&](const std::vector<float>& pcm) {
            for (std::size_t k = 0; k < pcm.size(); k += 2)
                audio.push_back(pcm[k]);
        });

    const auto total = static_cast<int>(seconds * inputRateHz);
    std::vector<std::complex<float>> stream(static_cast<std::size_t>(total));
    for (int n = 0; n < total; ++n) {
        const double t = static_cast<double>(n) / inputRateHz;
        const double env = 0.3 * (1.0 + modDepth * std::cos(2.0 * kPi * modHz * t));
        const double ph = 2.0 * kPi * carrierOffsetHz * t;
        // WIRE ORDER, matching runTone()'s convention.
        stream[static_cast<std::size_t>(n)] =
            std::complex<float>(static_cast<float>(env * std::cos(ph)),
                                static_cast<float>(env * std::sin(ph)));
    }

    constexpr std::size_t kFeedBlock = 126;   // arbitrary sub-block feed size
    for (std::size_t off = 0; off < stream.size(); off += kFeedBlock) {
        const std::size_t n = std::min(kFeedBlock, stream.size() - off);
        dsp.processIqBlock(std::vector<std::complex<float>>(
            stream.begin() + static_cast<std::ptrdiff_t>(off),
            stream.begin() + static_cast<std::ptrdiff_t>(off + n)));
    }
    QObject::disconnect(conn);

    AudioStats st;
    if (audio.empty())
        return st;
    const auto tail = std::min<std::size_t>(
        audio.size(), static_cast<std::size_t>(tailSeconds * audio.size() / seconds));
    const std::size_t start = audio.size() - tail;
    double sum = 0.0;
    for (std::size_t i = start; i < audio.size(); ++i) sum += audio[i];
    st.mean = sum / static_cast<double>(tail);
    double sumSq = 0.0;
    for (std::size_t i = start; i < audio.size(); ++i) {
        const double d = audio[i] - st.mean;
        sumSq += d * d;
    }
    st.acRms = std::sqrt(sumSq / static_cast<double>(tail));
    return st;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- Group 1: conjugation flips the spectral peak to the mirror bin ----
    // No WdspChannel, no radio, no hypothesis -- pure FFT/fftshift math.
    {
        constexpr int kFft = 64;
        constexpr double kCycles = 10.0;   // integer bin offset from DC

        AnanSpectrum specA(kFft);
        std::vector<float> binsA;
        check(specA.process(makeTone(kFft, kCycles, /*conjugate=*/false), binsA) == 1,
              "one frame produced for the direct tone");
        const int pA = peakBin(binsA);

        AnanSpectrum specB(kFft);
        std::vector<float> binsB;
        check(specB.process(makeTone(kFft, kCycles, /*conjugate=*/true), binsB) == 1,
              "one frame produced for the conjugated tone");
        const int pB = peakBin(binsB);

        std::fprintf(stderr, "peak bin: direct=%d conjugated=%d (fftSize=%d)\n",
                     pA, pB, kFft);
        check(pA != pB, "conjugating the tone moves the peak to a different bin");
        // fftshift centres DC at fftSize/2; a tone at +k cycles lands at
        // fftSize/2 + k, its conjugate (-k cycles) at fftSize/2 - k. The two
        // are symmetric about the centre bin, i.e. they sum to fftSize.
        check(pA + pB == kFft,
              "direct and conjugated peaks are mirror images about DC (pA+pB == fftSize)");
    }

    // ---- Group 2: bench-confirmed handedness pin (see file header) ----
    {
        AnanRxDsp dsp;
        AnanRxDsp::Config cfg;
        cfg.inputSampleRateHz = kInputRate;
        cfg.audioSampleRateHz = kAudioRate;
        cfg.dspBlockSize = kBlock;
        cfg.fftSize = 256;
        cfg.mode = WdspChannel::Mode::Lsb;
        cfg.filterLowHz = -9000.0;          // wide, so a shifted tone stays in band
        cfg.filterHighHz = -100.0;
        cfg.agcMode = 0;                    // AGC off: linear, nothing rescales
        cfg.maximumAgcGainDb = 40.0;
        cfg.blockForOutput = true;          // deterministic for an offline burst feed
        std::string err;
        check(dsp.configure(cfg, &err), err.empty() ? "AnanRxDsp configures" : err.c_str());

        const double base = runTone(dsp, 800.0, 0.0);
        std::fprintf(stderr, "offset 0 Hz -> audio %.0f Hz (expect ~800, bench-confirmed "
                             "handedness)\n", base);
        check(std::fabs(base - 800.0) < 120.0,
              "tone lands at 800 Hz with no shift, bench-confirmed handedness");

        const double up2k = runTone(dsp, 800.0, 2000.0);
        std::fprintf(stderr, "slice +2000 Hz -> audio %.0f Hz (expect ~2800)\n", up2k);
        check(std::fabs(up2k - 2800.0) < 200.0,
              "slice +2 kHz moves the tone to 2800 Hz");

        const double back = runTone(dsp, 800.0, 0.0);
        check(std::fabs(back - 800.0) < 120.0,
              "shift back to 0 restores the tone (stage is not accumulating)");
    }

    // ---- Group 3: AM DC-pedestal regression (confident -- a WDSP fact) ----
    {
        constexpr double kSeconds = 4.0;
        constexpr double kTail = 1.0;
        constexpr double kModHz = 400.0;
        constexpr double kModDepth = 0.5;

        AnanRxDsp::Config cfg;
        cfg.inputSampleRateHz = kInputRate;
        cfg.audioSampleRateHz = 24000;
        cfg.dspBlockSize = 1024;
        cfg.fftSize = 256;
        cfg.blockForOutput = true;
        cfg.mode = WdspChannel::Mode::Am;
        cfg.filterLowHz = -4000.0;
        cfg.filterHighHz = 4000.0;

        AnanRxDsp dsp;
        std::string err;
        check(dsp.configure(cfg, &err), err.empty() ? "AM configures" : err.c_str());
        const AudioStats st = runAm(dsp, kInputRate, kSeconds, kTail,
                                    /*carrierOffsetHz=*/1000.0, kModHz, kModDepth);
        check(st.acRms > 1e-4, "AM demod produced non-silent modulation");
        const double ratio = st.acRms > 0.0 ? std::fabs(st.mean) / st.acRms : 1e9;
        check(ratio < 0.10, "AM audio is zero-mean: no carrier DC pedestal survives");
        if (ratio >= 0.10) {
            std::fprintf(stderr, "  DC/AC ratio = %.3f (mean %.6f, acRms %.6f)\n",
                         ratio, st.mean, st.acRms);
        }

        // Unconfigured bypass -- r=0 would be a differentiator, not a no-op.
        AnanRxDsp::DcBlocker idle;
        bool passthrough = true;
        for (int n = 0; n < 256; ++n) {
            const auto x = static_cast<float>(std::cos(2.0 * kPi * n / 64.0));
            if (idle.process(x) != x) passthrough = false;
        }
        check(passthrough, "an unconfigured DC blocker passes its input through untouched");
    }

    // ---- Group 4: background-rebuild split (buildChannel/installRebuiltChannel) ----
    // Regression coverage for the zoom-freeze architecture fix: buildChannel()
    // must be usable standalone (it is called off AnanRxDsp's own thread in
    // production), and installRebuiltChannel() must re-apply the operator's
    // CURRENT mode/filter/AGC to the newly built channel, not whatever
    // buildChannel() happened to be given -- the staleness bug the fix closes.
    {
        AnanRxDsp::Config cfg;
        cfg.inputSampleRateHz = kInputRate;
        cfg.audioSampleRateHz = kAudioRate;
        cfg.dspBlockSize = kBlock;
        cfg.fftSize = 256;
        cfg.mode = WdspChannel::Mode::Usb;
        cfg.filterLowHz = 100.0;
        cfg.filterHighHz = 2900.0;
        cfg.agcMode = 3;
        cfg.maximumAgcGainDb = 40.0;
        cfg.blockForOutput = true;

        AnanRxDsp dsp;
        dsp.beginInitialBuild(cfg);
        AnanRxDsp::RebuildResult initial = AnanRxDsp::buildChannel(cfg);
        check(initial.channel != nullptr,
              initial.error.empty() ? "initial background build succeeds" : initial.error.c_str());
        check(dsp.installRebuiltChannel(std::move(initial)),
              "first-connect installs its asynchronously built channel");
        check(dsp.channelForTest()->config().mode == cfg.mode,
              "first-connect preserves the requested startup mode");
        check(dsp.channelForTest()->config().filterLowHz == cfg.filterLowHz
                  && dsp.channelForTest()->config().filterHighHz == cfg.filterHighHz,
              "first-connect preserves the requested startup filter");
        check(dsp.channelForTest()->config().agcMode == cfg.agcMode
                  && dsp.channelForTest()->config().maximumAgcGainDb
                         == cfg.maximumAgcGainDb,
              "first-connect preserves the requested startup AGC ceiling");
        const int firstId = dsp.channelForTest()->channelIdForTest();

        // Operator changes mode/filter/AGC on the FIRST channel -- this is
        // the state a rebuild swap must carry forward, not buildChannel()'s
        // own (still-USB, still-40dB) snapshot below.
        dsp.setMode(WdspChannel::Mode::Lsb);
        dsp.setFilter(-2900.0, -100.0);
        dsp.setAgc(2, 25.0);

        // installRebuiltChannel() on a failed/null result must be a clean
        // no-op: old channel untouched, false returned.
        AnanRxDsp::RebuildResult failed;
        failed.error = "synthetic failure for the test";
        check(!dsp.installRebuiltChannel(std::move(failed)),
              "installRebuiltChannel() returns false for a null-channel result");
        check(dsp.channelForTest()->channelIdForTest() == firstId,
              "a failed install leaves the existing channel untouched");
        check(dsp.channelForTest()->config().mode == WdspChannel::Mode::Lsb,
              "a failed install does not disturb the operator's mode change either");

        dsp.beginRebuild();

        // buildChannel() is static and thread-agnostic -- built here from a
        // config that does NOT reflect the operator's LSB/filter/AGC change
        // above (mirroring production: the background thread only ever sees
        // the rate-change snapshot, not live operator edits).
        AnanRxDsp::RebuildResult result = AnanRxDsp::buildChannel(cfg);
        check(result.channel != nullptr,
              result.error.empty() ? "buildChannel() succeeds" : result.error.c_str());

        check(dsp.installRebuiltChannel(std::move(result)),
              "installRebuiltChannel() returns true for a successful result");
        const WdspChannel* installed = dsp.channelForTest();
        check(installed->channelIdForTest() != firstId,
              "the installed channel is a genuinely different WdspChannel instance");
        check(installed->config().mode == WdspChannel::Mode::Lsb,
              "installRebuiltChannel() re-applies the operator's CURRENT mode, "
              "not buildChannel()'s (stale) USB snapshot");
        check(installed->config().filterLowHz == -2900.0
              && installed->config().filterHighHz == -100.0,
              "installRebuiltChannel() re-applies the operator's CURRENT filter edit");
        check(installed->config().agcMode == 2
              && installed->config().maximumAgcGainDb == 25.0,
              "installRebuiltChannel() re-applies the operator's CURRENT AGC setting");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "anan_rxdsp_handedness_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
