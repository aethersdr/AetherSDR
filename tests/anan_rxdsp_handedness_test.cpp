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
//   1. (Confident, no radio involved.) A WIRE-convention tone above centre
//      lands above centre, and one below lands below, through the whole
//      spectrum path -- AnanRxDsp feeding WDSP's analyzer, whose Spectrum0()
//      I/Q swap is now the one place the spectrum is mirrored. This also pins
//      the analyzer's point order (lowest frequency first) and that DC sits
//      at the centre point.
//   2. (CONFIRMED, 2026-08-21 -- was provisional, now pins a bench-verified
//      fact, not just current code behaviour.) AnanRxDsp's split
//      (demodulator raw, spectrum mirrored -- the same split Hl2RxDsp
//      settled on for Protocol 1; the spectrum's mirror is now WDSP's
//      analyzer's I/Q swap, see item 1) is the actually-correct one for a real
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

#include "core/backends/anan/AnanDroopCorrection.h"
#include "core/backends/anan/AnanRxDsp.h"
#include "core/backends/anan/AnanPanAnalyzer.h"
#include "core/dsp/WdspSMeter.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <functional>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <string>
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

// ---- Spectrum helpers: WDSP analyzer frames ----

int peakBin(const std::vector<float>& binsDbfs)
{
    int best = 0;
    float bestVal = binsDbfs.empty() ? 0.0f : binsDbfs[0];
    for (std::size_t i = 1; i < binsDbfs.size(); ++i) {
        if (binsDbfs[i] > bestVal) { bestVal = binsDbfs[i]; best = static_cast<int>(i); }
    }
    return best;
}

// Exactly enough samples for ONE analyzer FFT. Feeding a fresh analyzer this
// many produces exactly one FFT (its overlap advance at 48/96 ksps and 25 fps
// is far smaller than the FFT, so the ring never fills a second one), which
// makes the first frame deterministic: the seeded average IS that FFT.
constexpr int kOneFft = AnanPanAnalyzer::kMinFftSize;
constexpr int kPoints = 1024;

// A tone `offsetHz` above centre in WIRE convention -- exp(-jwt), the
// handedness a real HPSDR/ANAN radio sends for a signal above the dial.
std::vector<std::complex<float>> wireTone(int n, double offsetHz, int rateHz, float amp,
                                          int phaseOffset = 0)
{
    std::vector<std::complex<float>> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * offsetHz * (i + phaseOffset) / rateHz;
        v[static_cast<std::size_t>(i)] = {amp * static_cast<float>(std::cos(ph)),
                                          -amp * static_cast<float>(std::sin(ph))};
    }
    return v;
}

// The analyzer computes its FFTs on WDSP worker threads, so a frame is not
// synchronous with the feed. Feed `iq`, then keep pumping empty blocks (which
// only poll for a frame) until the first spectrumReady, or give up.
std::vector<float> firstFrame(AnanRxDsp& dsp, const std::vector<std::complex<float>>& iq)
{
    std::vector<float> frame;
    bool got = false;
    const auto conn = QObject::connect(&dsp, &AnanRxDsp::spectrumReady,
        [&](const std::vector<float>& bins) {
            if (!got) { frame = bins; got = true; }
        });
    dsp.processIqBlock(iq);
    const std::vector<std::complex<float>> none;
    QElapsedTimer t;
    t.start();
    while (!got && t.elapsed() < 10000) {
        QThread::msleep(2);
        dsp.processIqBlock(none);
    }
    QObject::disconnect(conn);
    return frame;
}

// The analyzer's own output for `iq`, with no AnanRxDsp involved -- the
// independent reference the droop groups compare against. Slot 60: channels
// only ever take slots 0..31, so this never collides with one.
std::vector<float> rawAnalyzerFrame(const std::vector<std::complex<float>>& iq, int rateHz,
                                    int points = kPoints)
{
    AnanPanAnalyzer::Settings s;
    s.sampleRateHz = rateHz;
    s.numPoints = points;
    s.framesPerSecond = 25;
    std::string err;
    auto a = AnanPanAnalyzer::create(60, s, &err);
    check(a != nullptr, err.empty() ? "reference analyzer is created" : err.c_str());
    if (!a)
        return {};
    a->feed(iq);
    std::vector<float> out;
    QElapsedTimer t;
    t.start();
    while (!a->takeFrame(out) && t.elapsed() < 10000)
        QThread::msleep(2);
    return out;
}

bool allClose(const std::vector<float>& a, const std::vector<float>& b, float tol)
{
    if (a.size() != b.size() || a.empty())
        return false;
    for (std::size_t k = 0; k < a.size(); ++k) {
        if (std::fabs(a[k] - b[k]) > tol)
            return false;
    }
    return true;
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
    AetherSDR::PcmFrame typed;
    int typedCount = 0;
    int legacyCount = 0;
    bool exact = true;
    const auto typedConn = QObject::connect(&dsp, &AnanRxDsp::pcmReady, &dsp,
        [&](const AetherSDR::PcmFrame& frame) { typed = frame; ++typedCount; });
    const auto conn = QObject::connect(&dsp, &AnanRxDsp::audioReady,
                     [&](const std::vector<float>& stereo) {
        ++legacyCount;
        exact = exact && typed.current() && typed.stream().format.sampleRateHz == kAudioRate
            && typed.samples().size() == static_cast<qsizetype>(stereo.size())
            && std::memcmp(typed.samples().constData(), stereo.data(), stereo.size() * sizeof(float)) == 0;
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
    QObject::disconnect(typedConn);
    check(exact && typedCount > 0 && typedCount == legacyCount,
          "ANAN typed output matches every legacy sample and actual producer rate");
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

    // Shared by the spectrum groups below: a spectrum-only configuration --
    // linear AGC so nothing else rescales, output blocking so the audio side
    // is deterministic too.
    const auto spectrumConfig = [](int rateHz) {
        AnanRxDsp::Config cfg;
        cfg.inputSampleRateHz = rateHz;
        cfg.audioSampleRateHz = kAudioRate;
        cfg.dspBlockSize = kBlock;
        cfg.panPoints = kPoints;   // one point per droop-table entry, as in production
        cfg.mode = WdspChannel::Mode::Usb;
        cfg.filterLowHz = 100.0;
        cfg.filterHighHz = 2900.0;
        cfg.agcMode = 0;
        cfg.maximumAgcGainDb = 40.0;
        cfg.blockForOutput = true;
        return cfg;
    };

    // ---- Group 1: a wire tone lands on the correct side of centre ----
    // The spectrum's mirror now happens inside WDSP's analyzer (Spectrum0()
    // swaps I and Q), and AnanRxDsp hands it the RAW wire. A wire-convention
    // tone 6 kHz above centre must come out 6 kHz above centre: about 128 of
    // 1024 points (46.875 Hz each at 48 ksps) above the centre point, 512.
    // Pins handedness, the analyzer's point order and where DC lands, all at
    // once; a conjugate added anywhere in the path puts it at 384 instead.
    {
        for (const double offsetHz : {6000.0, -6000.0}) {
            AnanRxDsp dsp;
            std::string err;
            check(dsp.configure(spectrumConfig(48000), &err),
                  err.empty() ? "handedness: configure() succeeds" : err.c_str());
            const std::vector<float> frame =
                firstFrame(dsp, wireTone(kOneFft, offsetHz, 48000, 0.25f));
            check(frame.size() == static_cast<std::size_t>(kPoints),
                  "handedness: one frame of kPoints points");
            const int peak = peakBin(frame);
            const int expected =
                kPoints / 2 + static_cast<int>(std::lround(offsetHz / 48000.0 * kPoints));
            std::fprintf(stderr, "wire tone %+.0f Hz: peak point %d, expected %d\n",
                         offsetHz, peak, expected);
            check(std::abs(peak - expected) <= 2,
                  offsetHz > 0 ? "a wire tone above centre lands above centre, at its frequency"
                               : "a wire tone below centre lands below centre, at its frequency");
        }
    }

    // ---- Group 1b: the analyzer's settings, and a seeded first frame ----
    {
        // deskHPSDR's formulas, as AnanPanAnalyzer::derive() applies them.
        AnanPanAnalyzer::Settings s;
        s.sampleRateHz = 48000;
        s.numPoints = 1024;
        s.framesPerSecond = 25;
        s.averageTimeMs = 250;   // deskHPSDR's default
        AnanPanAnalyzer::Derived d = AnanPanAnalyzer::derive(s);
        check(d.fftSize == 16384, "the FFT is never smaller than 16384");
        check(d.overlap == 16384 - 48000 / 25,
              "overlap = FFT - rate/fps: one FFT per display frame over fresh samples");
        check(d.maxWriteahead == 16384 + 4800,
              "write-ahead = FFT + the lesser of 0.1 s of input and 0.1 s of FFT work");
        check(std::fabs(d.avBackmult - std::exp(-1.0 / (25 * 0.25))) < 1e-12,
              "history weight is deskHPSDR's exp(-1/(fps*t)): 250 ms at 25 fps");
        s.averageTimeMs = 0;
        check(AnanPanAnalyzer::derive(s).avBackmult == 0.0,
              "FFT AVG 0 means no time averaging");
        s.sampleRateHz = 1536000;
        s.averageTimeMs = 250;
        d = AnanPanAnalyzer::derive(s);
        check(d.overlap == 0, "at 1536 ksps the FFTs tile the stream with no overlap");
        // 1536000 / 16384 = 93.75 FFTs per second, each applying the weight, so
        // the weight must be derived from that rate or 250 ms would deliver
        // ~67 ms.
        check(std::fabs(d.avBackmult - std::exp(-1.0 / (93.75 * 0.25))) < 1e-12,
              "history weight follows the analyzer's FFT rate once the overlap saturates, "
              "so FFT AVG keeps its 10 ms unit at 1536 ksps");
        s.averageTimeMs = 0;
        // derive() grows the FFT for a larger point count, but create() refuses
        // anything above WDSP's dMAX_PIXELS -- pin the refusal that is actually
        // reachable alongside the formula.
        s.numPoints = 20000;
        check(AnanPanAnalyzer::derive(s).fftSize == 32768,
              "derive(): the FFT grows to the next power of two above the point count");
        {
            std::string tooMany;
            check(AnanPanAnalyzer::create(61, s, &tooMany) == nullptr && !tooMany.empty(),
                  "create() refuses a point count above WDSP's dMAX_PIXELS");
        }
        s.numPoints = 1024;
        s.framesPerSecond = 1000;
        d = AnanPanAnalyzer::derive(s);
        check(d.maxWriteahead <= 2 * AnanPanAnalyzer::kMaxFftSize - AnanPanAnalyzer::kBlockSize,
              "write-ahead never exceeds the analyzer's input ring");

        // Seeding. WDSP's log-recursive average starts at -160 dB; unseeded,
        // the first frame after every build (every zoom) would read ~130 dB low
        // and ramp up over a second. Seeded, the first frame IS the first FFT,
        // and a later frame of the same steady tone reads the same.
        AnanRxDsp dsp;
        std::string err;
        AnanRxDsp::Config seedCfg = spectrumConfig(48000);
        seedCfg.spectrumAverageMs = 250;   // averaging ON: the seed matters
        check(dsp.configure(seedCfg, &err),
              err.empty() ? "seeding: configure() succeeds" : err.c_str());
        const auto tone = wireTone(2 * kOneFft, 6000.0, 48000, 0.25f);
        const std::vector<std::complex<float>> firstHalf(tone.begin(), tone.begin() + kOneFft);
        const std::vector<std::complex<float>> secondHalf(tone.begin() + kOneFft, tone.end());
        const std::vector<float> f1 = firstFrame(dsp, firstHalf);
        const std::vector<float> f2 = firstFrame(dsp, secondHalf);
        check(f1.size() == static_cast<std::size_t>(kPoints)
                  && f2.size() == static_cast<std::size_t>(kPoints),
              "seeding: two frames produced");
        if (f1.size() == f2.size() && !f1.empty()) {
            const int pk = peakBin(f1);
            std::fprintf(stderr, "seeding: first frame peak %.2f dB, later frame %.2f dB\n",
                         f1[static_cast<std::size_t>(pk)], f2[static_cast<std::size_t>(pk)]);
            check(f1[static_cast<std::size_t>(pk)] > -40.0f,
                  "the first frame reads the tone at its level, not ramping up from -160 dB");
            check(std::fabs(f2[static_cast<std::size_t>(pk)] - f1[static_cast<std::size_t>(pk)])
                      < 1.0f,
                  "a later frame of the same steady tone reads the same -- no ramp");
        }

        // Turning FFT AVG up from 0 mid-stream: entering log-recursive mode
        // resets WDSP's history to -160 dB, so this must re-seed too.
        AnanRxDsp live;
        check(live.configure(spectrumConfig(48000), &err),
              err.empty() ? "re-seed: configure() succeeds" : err.c_str());
        const std::vector<float> g1 = firstFrame(live, firstHalf);   // averaging off
        live.setSpectrumAverageMs(250);
        const std::vector<float> g2 = firstFrame(live, secondHalf);  // averaging on
        if (g1.size() == g2.size() && !g1.empty()) {
            const int pk = peakBin(g1);
            std::fprintf(stderr, "re-seed: averaging off %.2f dB, switched on %.2f dB\n",
                         g1[static_cast<std::size_t>(pk)], g2[static_cast<std::size_t>(pk)]);
            check(std::fabs(g2[static_cast<std::size_t>(pk)] - g1[static_cast<std::size_t>(pk)])
                      < 1.0f,
                  "switching averaging on mid-stream re-seeds -- no fade-in from -160 dB");
        } else {
            check(false, "re-seed: two frames produced");
        }

        // Switching the averaging MODE (weighted-average toggle) resets WDSP's
        // history as well, so it must re-seed too.
        const auto more = wireTone(kOneFft, 6000.0, 48000, 0.25f, 2 * kOneFft);
        live.setSpectrumLogAverage(false);
        const std::vector<float> g3 = firstFrame(live, more);
        if (g3.size() == g1.size() && !g1.empty()) {
            const int pk = peakBin(g1);
            std::fprintf(stderr, "re-seed: switched to linear %.2f dB\n",
                         g3[static_cast<std::size_t>(pk)]);
            check(std::fabs(g3[static_cast<std::size_t>(pk)] - g1[static_cast<std::size_t>(pk)])
                      < 1.0f,
                  "switching log -> linear averaging mid-stream re-seeds -- no dip");
        } else {
            check(false, "re-seed: a frame after the mode switch");
        }
    }

    // ---- Group 1c: the seed must come from a frame computed AFTER the switch ----
    // In production takeFrame() only polls when a display frame is due (the
    // 25 fps cap), so a frame the worker finished under the OLD mode can be
    // waiting in GetPixels when FFT AVG is switched on. Seeding from that frame
    // would leave the new mode's -160 dB history in place and fade the
    // panadapter in from black -- the exact defect the seed exists to prevent.
    {
        AnanRxDsp capped;
        std::string err;
        check(capped.configure(spectrumConfig(48000), &err),
              err.empty() ? "capped seed: configure() succeeds" : err.c_str());
        capped.setSpectrumRateFps(25);   // what AnanBackend::setPanFrameRate() does
        const auto steady = wireTone(2 * kOneFft, 6000.0, 48000, 0.25f);
        const std::vector<std::complex<float>> first(steady.begin(), steady.begin() + kOneFft);
        const std::vector<float> c1 = firstFrame(capped, first);   // averaging off
        check(c1.size() == static_cast<std::size_t>(kPoints), "capped seed: a first frame");
        // One more FFT's worth (the overlap advance is 1920 samples at 48 ksps
        // and 25 fps): computed in mode "none" by the worker, NOT taken -- the
        // next display frame is not due yet.
        const std::vector<std::complex<float>> more(steady.begin() + kOneFft,
                                                    steady.begin() + kOneFft + 2048);
        capped.processIqBlock(more);
        QThread::msleep(20);
        capped.setSpectrumAverageMs(250);
        // Poll without feeding for longer than a display interval. Without the
        // drain in AnanPanAnalyzer::applyAveraging() this takes the stale
        // mode-none frame and marks the average seeded.
        {
            const std::vector<std::complex<float>> none;
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 120) {
                QThread::msleep(2);
                capped.processIqBlock(none);
            }
        }
        const std::vector<std::complex<float>> after(steady.begin() + kOneFft + 2048,
                                                     steady.begin() + kOneFft + 4096);
        const std::vector<float> c2 = firstFrame(capped, after);   // first frame computed in log mode
        if (c1.size() == c2.size() && !c1.empty()) {
            const int pk = peakBin(c1);
            std::fprintf(stderr, "capped seed: before %.2f dB, first averaged frame %.2f dB\n",
                         c1[static_cast<std::size_t>(pk)], c2[static_cast<std::size_t>(pk)]);
            check(std::fabs(c2[static_cast<std::size_t>(pk)] - c1[static_cast<std::size_t>(pk)])
                      < 1.0f,
                  "a frame pending from before the switch does not seed the new mode -- "
                  "no fade-in from -160 dB under the display-rate cap");
        } else {
            check(false, "capped seed: a frame after the switch");
        }
    }

    // ---- Group 1d: the point count follows the panel width ----
    // A 1917-pixel panel with the 4% edge crop asks for 2083 points
    // (panPointsForPixelWidth()). The tone must still land at its frequency
    // on the wider grid -- point i sits at fraction i / (N - 1) of the span --
    // both when the count is built in and when it changes mid-stream, and a
    // mid-stream change must re-seed rather than blend two point grids.
    {
        constexpr int kWide = 2083;
        const auto expectedAt = [](int points, double offsetHz) {
            return static_cast<int>(std::lround((points - 1) * (0.5 + offsetHz / 48000.0)));
        };

        AnanRxDsp built;
        std::string err;
        AnanRxDsp::Config wideCfg = spectrumConfig(48000);
        wideCfg.panPoints = kWide;
        check(built.configure(wideCfg, &err),
              err.empty() ? "width: configure() at 2083 points succeeds" : err.c_str());
        const std::vector<float> w1 = firstFrame(built, wireTone(kOneFft, 6000.0, 48000, 0.25f));
        check(w1.size() == static_cast<std::size_t>(kWide), "width: a 2083-point frame");
        std::fprintf(stderr, "width: built at %d points, peak %d, expected %d\n",
                     kWide, peakBin(w1), expectedAt(kWide, 6000.0));
        check(std::abs(peakBin(w1) - expectedAt(kWide, 6000.0)) <= 2,
              "at 2083 points a wire tone lands at its frequency");

        // Mid-stream, averaging on: 1024 points, then the panel reports its
        // width. The next frame is the new count, at the tone's frequency and
        // level -- seeded, not ramping up from WDSP's -160 dB.
        AnanRxDsp live;
        AnanRxDsp::Config liveCfg = spectrumConfig(48000);
        liveCfg.spectrumAverageMs = 250;
        check(live.configure(liveCfg, &err),
              err.empty() ? "width: live configure() succeeds" : err.c_str());
        const auto tone = wireTone(2 * kOneFft, 6000.0, 48000, 0.25f);
        const std::vector<std::complex<float>> firstHalf(tone.begin(), tone.begin() + kOneFft);
        const std::vector<std::complex<float>> secondHalf(tone.begin() + kOneFft, tone.end());
        const std::vector<float> before = firstFrame(live, firstHalf);
        live.setPanPoints(kWide);
        const std::vector<float> after = firstFrame(live, secondHalf);
        check(before.size() == static_cast<std::size_t>(kPoints)
                  && after.size() == static_cast<std::size_t>(kWide),
              "width: setPanPoints() changes the next frame's point count");
        if (!before.empty() && after.size() == static_cast<std::size_t>(kWide)) {
            const int pk = peakBin(after);
            std::fprintf(stderr, "width: mid-stream 1024 -> %d, peak %d (%.2f dB), expected %d; "
                         "1024-point peak %.2f dB\n", kWide, pk,
                         after[static_cast<std::size_t>(pk)], expectedAt(kWide, 6000.0),
                         before[static_cast<std::size_t>(peakBin(before))]);
            check(std::abs(pk - expectedAt(kWide, 6000.0)) <= 2,
                  "after a mid-stream width change the tone still lands at its frequency");
            check(after[static_cast<std::size_t>(pk)] > -40.0f,
                  "a mid-stream width change re-seeds the average -- no fade-in from -160 dB");
        }
        live.setPanPoints(1);
        const auto more = wireTone(kOneFft, 6000.0, 48000, 0.25f, 2 * kOneFft);
        check(firstFrame(live, more).size() == static_cast<std::size_t>(kWide),
              "a point count below 2 is ignored");

    }

    // ---- Group 2: bench-confirmed handedness pin (see file header) ----
    {
        AnanRxDsp dsp;
        AnanRxDsp::Config cfg;
        cfg.inputSampleRateHz = kInputRate;
        cfg.audioSampleRateHz = kAudioRate;
        cfg.dspBlockSize = kBlock;
        cfg.panPoints = 256;
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
        cfg.panPoints = 256;
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
        cfg.panPoints = 256;
        cfg.mode = WdspChannel::Mode::Usb;
        cfg.filterLowHz = 100.0;
        cfg.filterHighHz = 2900.0;
        cfg.agcMode = 3;
        cfg.maximumAgcGainDb = 40.0;
        cfg.blockForOutput = true;
        cfg.noiseBlankerEnabled = true;
        cfg.noiseBlankerLevel = 50;

        AnanRxDsp dsp;
        dsp.beginInitialBuild(cfg);
        AnanRxDsp::RebuildResult initial = AnanRxDsp::buildChannel(cfg);
        check(initial.channel != nullptr,
              initial.error.empty() ? "initial background build succeeds" : initial.error.c_str());
        // Read BEFORE the install: installChannel() re-applies m_config's
        // blanker anyway, so only the built channel shows buildChannel() did.
        check(initial.channel && initial.channel->noiseBlankerEnabled()
                  && initial.channel->config().noiseBlankerLevel == 50,
              "buildChannel() opens the channel with the requested noise blanker");
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
        // The operator moves the NB button WHILE the background build runs.
        // Deferred, not pushed: the swap below has to apply it.
        dsp.setNoiseBlanker(false, 80);

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
        check(!installed->noiseBlankerEnabled()
                  && installed->config().noiseBlankerLevel == 80,
              "installRebuiltChannel() re-applies a noise blanker change made "
              "during the build, not buildChannel()'s (stale) NB-on snapshot");

        // Live, with no rebuild in flight, the change reaches the channel.
        dsp.setNoiseBlanker(true, 120);
        check(dsp.channelForTest()->noiseBlankerEnabled()
                  && dsp.channelForTest()->config().noiseBlankerLevel == 100,
              "setNoiseBlanker() reaches the live channel, level clamped to 100");
    }

    // ---- Group 4b: a mute must not de-arm the noise blanker ----
    // setAudioMuted() clocks the channel with zeros (a rate change's settle
    // window), and the blanker triggers on a RATIO against a running average
    // magnitude. Unheld, that average decays to nothing during the silence and
    // the first real audio afterwards is gated as if it were an impulse.
    // Mirrors hl2_noise_blanker_test's section 5 and measures it the same way:
    // blanker-on recovery against blanker-off recovery, in blocks, so the
    // pipeline's own settling (paid by both arms) cancels out.
    {
        constexpr int kEdgeBlock = 256;   // short, so a ~7 ms hole spans blocks
        constexpr double kToneHz = 1000.0;
        constexpr double kToneAmp = 0.02;
        constexpr int kNbLevel = 80;

        // WIRE-convention tone, demodulated in LSB -- Group 2's geometry.
        auto tone = [&](int samples, int phaseOffset) {
            std::vector<std::complex<float>> out(static_cast<std::size_t>(samples));
            for (int n = 0; n < samples; ++n) {
                const double ph = 2.0 * kPi * kToneHz * (n + phaseOffset) / kInputRate;
                out[static_cast<std::size_t>(n)] = {
                    static_cast<float>(kToneAmp * std::cos(ph)),
                    static_cast<float>(kToneAmp * std::sin(ph))
                };
            }
            return out;
        };
        auto feed = [&](AnanRxDsp& dsp, const std::vector<std::complex<float>>& s) {
            for (std::size_t off = 0; off < s.size(); off += kEdgeBlock) {
                const std::size_t n = std::min<std::size_t>(kEdgeBlock, s.size() - off);
                dsp.processIqBlock(std::vector<std::complex<float>>(
                    s.begin() + static_cast<std::ptrdiff_t>(off),
                    s.begin() + static_cast<std::ptrdiff_t>(off + n)));
            }
        };

        int recoveryBlockOff = -1;
        int recoveryBlockOn = -1;
        for (int pass = 0; pass < 2; ++pass) {
            const bool blankerOn = (pass == 1);
            AnanRxDsp::Config cfg;
            cfg.inputSampleRateHz = kInputRate;
            cfg.audioSampleRateHz = kAudioRate;
            cfg.dspBlockSize = kEdgeBlock;
            cfg.panPoints = 256;
            cfg.mode = WdspChannel::Mode::Lsb;
            cfg.filterLowHz = -3000.0;
            cfg.filterHighHz = -150.0;
            cfg.agcMode = 0;               // AGC off: it would hide the hole
            cfg.maximumAgcGainDb = 40.0;
            cfg.blockForOutput = true;
            AnanRxDsp dsp;
            std::string err;
            check(dsp.configure(cfg, &err),
                  err.empty() ? "AnanRxDsp configures for the mute-edge case" : err.c_str());
            if (blankerOn)
                dsp.setNoiseBlanker(true, kNbLevel);

            std::vector<double> blockRms;
            bool collecting = false;
            const auto conn = QObject::connect(&dsp, &AnanRxDsp::audioReady, &dsp,
                [&](const std::vector<float>& pcm) {
                    if (!collecting)
                        return;
                    double sum = 0.0;
                    std::size_t count = 0;
                    for (std::size_t k = 0; k < pcm.size(); k += 2) {
                        sum += static_cast<double>(pcm[k]) * pcm[k];
                        ++count;
                    }
                    blockRms.push_back(count > 0 ? std::sqrt(sum / static_cast<double>(count))
                                                 : 0.0);
                });

            feed(dsp, tone(kInputRate, 0));            // arm on real signal
            dsp.setAudioMuted(true);
            feed(dsp, tone(kInputRate / 2, kInputRate)); // muted: channel sees zeros
            dsp.setAudioMuted(false);
            collecting = true;
            feed(dsp, tone(kInputRate / 2, kInputRate + kInputRate / 2));
            QObject::disconnect(conn);

            double steady = 0.0;
            const std::size_t tail = std::min<std::size_t>(10, blockRms.size());
            for (std::size_t k = blockRms.size() - tail; k < blockRms.size(); ++k)
                steady += blockRms[k];
            steady = tail > 0 ? steady / static_cast<double>(tail) : 0.0;
            int recovered = -1;
            for (std::size_t k = 0; k < blockRms.size(); ++k) {
                if (blockRms[k] > steady * 0.5) {
                    recovered = static_cast<int>(k);
                    break;
                }
            }
            (blankerOn ? recoveryBlockOn : recoveryBlockOff) = recovered;
        }

        std::fprintf(stderr, "post-mute recovery block off=%d on=%d (%d samples each)\n",
                     recoveryBlockOff, recoveryBlockOn, kEdgeBlock);
        check(recoveryBlockOff >= 0, "the blanker-off arm recovers audio after a mute");
        check(recoveryBlockOn >= 0, "the blanker-on arm recovers audio after a mute");
        check(recoveryBlockOn >= 0 && recoveryBlockOn <= recoveryBlockOff,
              "the noise blanker adds NO delay to the return of audio after a mute");
    }

    // ---- Group 5: droop-correction insertion point ----
    // Pins that AnanDroopCorrection's per-point dB correction lands on the
    // analyzer's output, once, and that the edge fade follows it. Uses a
    // SYNTHETIC table pushed via setDroopCorrectionTable() rather than any
    // bench-measured one, so this test is independent of whatever a real
    // calibration sweep or a per-radio settings load happened to produce. The
    // reference is a standalone analyzer fed the same IQ -- no AnanRxDsp
    // involved -- so the first frame's emitted points must equal that raw
    // frame plus the synthetic table, point for point.
    {
        AnanRxDsp dsp;
        std::string err;
        check(dsp.configure(spectrumConfig(48000), &err),
              err.empty() ? "droop-correction test: configure() succeeds" : err.c_str());

        // Synthetic, deliberately non-zero and non-uniform so the test can't
        // pass by accident on an all-zero table.
        DroopCorrectionTable syntheticTable{};
        for (std::size_t k = 0; k < syntheticTable.size(); ++k)
            syntheticTable[k] = 3.25f + 0.01f * static_cast<float>(k % 50);
        dsp.setDroopCorrectionTable(48,
            std::vector<float>(syntheticTable.begin(), syntheticTable.end()));

        const auto iq = wireTone(kOneFft, 1300.0, 48000, 0.25f);
        const std::vector<float> emitted = firstFrame(dsp, iq);
        check(!emitted.empty(), "the droop test's AnanRxDsp emits a spectrum frame");
        const std::vector<float> rawBins = rawAnalyzerFrame(iq, 48000);

        const DroopCorrectionTable& table = syntheticTable;
        check(emitted.size() == rawBins.size() && emitted.size() == table.size(),
              "emitted/reference/table sizes all agree");

        // applyEdgeFade() runs right after applyDroopCorrectionDb() for any
        // rate with a real (non-zero) table -- which this synthetic one is --
        // so the outermost kTailBins on each side are the fade's own curve.
        // kTailBins mirrors applyEdgeFade()'s default tailFraction (0.03) at
        // 1024 points: static_cast<size_t>(1024 * 0.03f) == 30.
        constexpr std::size_t kTailBins = 30;

        std::vector<float> rawPlusTable(rawBins.size());
        for (std::size_t k = 0; k < rawPlusTable.size(); ++k)
            rawPlusTable[k] = rawBins[k] + table[k];

        bool matched = emitted.size() == rawBins.size() && emitted.size() == table.size();
        for (std::size_t k = kTailBins; matched && k < emitted.size() - kTailBins; ++k) {
            if (std::fabs(emitted[k] - rawPlusTable[k]) > 1.0e-3f) {
                matched = false;
                std::fprintf(stderr,
                    "  point %zu: emitted=%.6f raw+correction=%.6f (raw=%.6f correction=%.6f)\n",
                    k, emitted[k], rawPlusTable[k], rawBins[k], table[k]);
            }
        }
        check(matched,
              "the first emitted frame's non-tail points equal the analyzer's raw points "
              "plus the rate's droop correction table -- the correction lands once, on "
              "the analyzer's output");

        // Tail points: applyEdgeFade() run on a copy of raw+table, at the
        // same defaults processIqBlock() uses, must equal what was emitted --
        // the fade is the SECOND step, not a replacement for the correction.
        std::vector<float> expectedTail = rawPlusTable;
        applyEdgeFade(expectedTail);
        bool tailMatched = expectedTail.size() == emitted.size();
        for (std::size_t k = 0; tailMatched && k < kTailBins; ++k) {
            if (std::fabs(emitted[k] - expectedTail[k]) > 1.0e-3f)
                tailMatched = false;
            const std::size_t ridx = emitted.size() - 1 - k;
            if (std::fabs(emitted[ridx] - expectedTail[ridx]) > 1.0e-3f)
                tailMatched = false;
        }
        check(tailMatched,
              "the tail points match applyEdgeFade() run on raw+table -- the cosmetic "
              "fade is the second step, not a replacement for the real correction");
    }

    // ---- Group 5b: the droop correction still lands at a panel-width count ----
    // The tables stay at 1024 points. At any other count an exact-size apply
    // would skip the correction without a word and bring the roll-off back,
    // so the emitted frame must equal the analyzer's raw frame at that count
    // plus the table read onto it (applyDroopCorrectionDbResampled()).
    {
        constexpr int kWide = 2083;
        AnanRxDsp dsp;
        std::string err;
        AnanRxDsp::Config cfg = spectrumConfig(48000);
        cfg.panPoints = kWide;
        check(dsp.configure(cfg, &err),
              err.empty() ? "droop at width: configure() succeeds" : err.c_str());
        DroopCorrectionTable table{};
        for (std::size_t k = 0; k < table.size(); ++k)
            table[k] = 3.25f + 0.01f * static_cast<float>(k % 50);
        dsp.setDroopCorrectionTable(48, std::vector<float>(table.begin(), table.end()));

        const auto iq = wireTone(kOneFft, 1300.0, 48000, 0.25f);
        const std::vector<float> emitted = firstFrame(dsp, iq);
        const std::vector<float> raw = rawAnalyzerFrame(iq, 48000, kWide);
        check(emitted.size() == static_cast<std::size_t>(kWide) && raw.size() == emitted.size(),
              "droop at width: emitted and reference frames are both 2083 points");

        std::vector<float> expected = raw;
        applyDroopCorrectionDbResampled(expected, table);
        applyEdgeFade(expected);
        bool matched = expected.size() == emitted.size() && !emitted.empty();
        for (std::size_t k = 0; matched && k < emitted.size(); ++k) {
            if (std::fabs(emitted[k] - expected[k]) > 1.0e-3f) {
                matched = false;
                std::fprintf(stderr, "  point %zu: emitted=%.6f expected=%.6f\n",
                             k, emitted[k], expected[k]);
            }
        }
        check(matched,
              "at 2083 points the emitted frame is the raw frame plus the resampled table, "
              "then the edge fade -- the correction is not skipped");
        // Non-vacuous: the resampled table is not zero, so a skipped
        // correction could not match.
        check(!raw.empty() && std::fabs(emitted[kWide / 2] - raw[kWide / 2]) > 3.0f,
              "droop at width: the correction actually moved the centre point");
    }

    // ---- Group 6: rate change picks up the NEW rate's droop table ----
    // Regression for a real bug: installRebuiltChannel() swapped in the new
    // WdspChannel and spectrum stage but never updated m_config.inputSampleRateHz,
    // so droopTableForRate() (which reads m_config.inputSampleRateHz, not
    // the rate the new channel was actually built for) kept consulting
    // whatever rate was live at the last configure() -- forever, for every
    // live rate change after the first. On real hardware this meant a live
    // panadapter zoom (which snaps to a new DDC0 rate and rebuilds through
    // exactly this path) kept applying the CONNECT-TIME rate's correction
    // curve to a different rate's spectrum: visible as a mismatched, lumpy
    // rolloff rather than a flat corrected trace. Configure at 48 ksps,
    // rebuild to 96 ksps like a live rate change does, and confirm the
    // emitted spectrum reflects the 96 ksps table -- not the 48 ksps one and
    // not the zero fallback.
    {
        const AnanRxDsp::Config cfg48 = spectrumConfig(48000);   // -> 48 ksps

        AnanRxDsp dsp;
        std::string err;
        check(dsp.configure(cfg48, &err),
              err.empty() ? "rate-change test: initial 48 ksps configure() succeeds"
                          : err.c_str());

        DroopCorrectionTable table48{};
        DroopCorrectionTable table96{};
        for (std::size_t k = 0; k < table48.size(); ++k) {
            table48[k] = 3.25f + 0.01f * static_cast<float>(k % 50);
            table96[k] = 9.0f + 0.02f * static_cast<float>(k % 37);   // distinct shape
        }
        dsp.setDroopCorrectionTable(48, std::vector<float>(table48.begin(), table48.end()));
        dsp.setDroopCorrectionTable(96, std::vector<float>(table96.begin(), table96.end()));

        // Rebuild to 96 ksps exactly as AnanBackend::beginRateChange() does:
        // buildChannel() off this object's own thread, then
        // installRebuiltChannel() to swap it in.
        AnanRxDsp::Config cfg96 = cfg48;
        cfg96.inputSampleRateHz = 96000;   // -> 96 ksps
        AnanRxDsp::RebuildResult result = AnanRxDsp::buildChannel(cfg96);
        check(result.channel != nullptr && result.analyzer != nullptr,
              result.error.empty() ? "rate-change test: 96 ksps buildChannel() succeeds"
                                   : result.error.c_str());
        check(dsp.installRebuiltChannel(std::move(result)),
              "rate-change test: installRebuiltChannel() to 96 ksps succeeds");

        const auto iq = wireTone(kOneFft, 1300.0, 96000, 0.25f);
        const std::vector<float> emitted = firstFrame(dsp, iq);
        check(!emitted.empty(), "rate-change test: a frame is emitted after the rebuild");
        const std::vector<float> rawBins = rawAnalyzerFrame(iq, 96000);

        // Skip the tail points -- applyEdgeFade() replaces them with its own
        // curve (Group 5 covers that); this group's job is only to confirm the
        // RIGHT rate's table drives the rest after a live rate change.
        constexpr std::size_t kTailBins = 30;
        bool matched = emitted.size() == rawBins.size() && emitted.size() == table96.size();
        for (std::size_t k = kTailBins; matched && k < emitted.size() - kTailBins; ++k) {
            if (std::fabs(emitted[k] - (rawBins[k] + table96[k])) > 1.0e-3f)
                matched = false;
        }
        check(matched,
              "after a live rate change, the emitted frame's non-tail points use the NEW "
              "rate's (96 ksps) droop table -- proving m_config.inputSampleRateHz was "
              "updated by the rebuild, not left stale at the connect-time 48 ksps");
    }

    // ---- Group 7: the sweep bypass, and forgetting a radio's tables ----
    // Two lifecycle rules the droop feature depends on, both invisible to
    // Groups 5 and 6 because both of those only ever measure the armed path.
    //
    // (a) BYPASS. AnanDroopCalibrator taps the same spectrumReady bins the
    //     panadapter paints, so it measures its own corrected output unless
    //     the correction is suspended for the sweep. The subtle half is the
    //     EDGE FADE: processIqBlock() decides whether to fade by testing
    //     `&droopTable != &kDroopCorrectionZero`, an IDENTITY test -- so
    //     "bypass" implemented as "push an all-zero table" would store a
    //     copy, keep failing that test, and leave the synthetic fade running
    //     to be measured as if it were hardware droop. The pin below is
    //     therefore equality with the RAW reference across the WHOLE frame,
    //     tails included, not just the non-tail bins the other groups check.
    //
    // (b) CLEAR. m_droopTables outlives any one connection (this object is
    //     constructed once per backend), so a second radio in the same
    //     session would render through the first one's corrections. Clearing
    //     must return the same rate to the true zero path -- again including
    //     the fade, for the same identity reason.
    //
    // Each case needs its OWN AnanRxDsp: the first frame from a fresh analyzer
    // is exactly one FFT (the average is seeded from it), which is what makes
    // an exact comparison against a raw reference possible.
    {
        constexpr int kRateKsps = 48;
        const AnanRxDsp::Config cfg = spectrumConfig(kRateKsps * 1000);

        // Non-uniform and non-zero, so "suppressed" cannot pass by accident.
        DroopCorrectionTable syntheticTable{};
        for (std::size_t k = 0; k < syntheticTable.size(); ++k)
            syntheticTable[k] = 5.5f + 0.02f * static_cast<float>(k % 40);
        const std::vector<float> tableVec(syntheticTable.begin(), syntheticTable.end());

        const auto iq = wireTone(kOneFft, 1900.0, kRateKsps * 1000, 0.25f);

        // The raw reference: a standalone analyzer, no AnanRxDsp involved.
        const std::vector<float> rawBins = rawAnalyzerFrame(iq, kRateKsps * 1000);

        // Captures the first emitted frame from a freshly configured AnanRxDsp
        // after `arrange` has had its way with the droop tables.
        const auto firstFrameWith = [&](const std::function<void(AnanRxDsp&)>& arrange) {
            AnanRxDsp dsp;
            std::string err;
            check(dsp.configure(cfg, &err),
                  err.empty() ? "bypass test: configure() succeeds" : err.c_str());
            dsp.setDroopCorrectionTable(kRateKsps, tableVec);
            arrange(dsp);
            const std::vector<float> emitted = firstFrame(dsp, iq);
            check(!emitted.empty(), "bypass test: processIqBlock() emits a spectrum frame");
            return emitted;
        };

        // `report` only for the checks that EXPECT equality -- the negative
        // controls below call this too, and printing their first differing
        // point would decorate a fully passing run with what looks like
        // failure output.
        const auto equalsRawEverywhere = [&](const std::vector<float>& emitted,
                                             bool report) {
            if (emitted.size() != rawBins.size() || emitted.empty())
                return false;
            for (std::size_t k = 0; k < emitted.size(); ++k) {
                if (std::fabs(emitted[k] - rawBins[k]) > 1.0e-3f) {
                    if (report)
                        std::fprintf(stderr, "  point %zu: emitted=%.6f raw=%.6f\n",
                                     k, emitted[k], rawBins[k]);
                    return false;
                }
            }
            return true;
        };

        // Control: armed, the table really does change the points. Without this
        // the two suppression checks below would pass on a broken table push.
        const std::vector<float> armed = firstFrameWith([](AnanRxDsp&) {});
        check(!equalsRawEverywhere(armed, /*report=*/false),
              "control: with the table armed, the emitted frame differs from raw");

        const std::vector<float> bypassed =
            firstFrameWith([](AnanRxDsp& d) { d.setDroopCorrectionBypassed(true); });
        check(equalsRawEverywhere(bypassed, /*report=*/true),
              "with the correction bypassed, the emitted frame equals the analyzer's raw "
              "points across the WHOLE frame -- neither the per-point correction nor the "
              "edge fade survives, so a calibration sweep measures the radio and not its "
              "own corrected output");

        const std::vector<float> cleared =
            firstFrameWith([](AnanRxDsp& d) { d.clearDroopCorrectionTables(); });
        check(equalsRawEverywhere(cleared, /*report=*/true),
              "after clearDroopCorrectionTables(), the same rate falls back to the true "
              "zero path (fade included) -- a second radio in one session cannot inherit "
              "the first one's corrections");

        // The bypass is a sweep property, not a table property: lifting it
        // must bring the SAME tables back without re-pushing them.
        const std::vector<float> restored = firstFrameWith([](AnanRxDsp& d) {
            d.setDroopCorrectionBypassed(true);
            d.setDroopCorrectionBypassed(false);
        });
        check(!equalsRawEverywhere(restored, /*report=*/false)
                  && allClose(restored, armed, 1.0e-3f),
              "lifting the bypass restores the identical corrected frame -- the tables "
              "were hidden, never discarded, so an aborted sweep cannot lose a "
              "calibration");
    }

    // ---- Group 8: the S-meter never publishes our own silence ----
    //
    // Two edges, one cause. While the audio channel is clocked with zeros
    // (setAudioMuted(), used across a rate change) WDSP's signal-average
    // meter integrates that silence, and because nothing flushes its average
    // the reading is still wrong for the length of its own time constant
    // AFTER the mute lifts. A guard on the mute flag alone would therefore
    // move the needle's dive from during the zoom to the end of it. Removing
    // the settle window turns the last check here red.
    {
        const int settle = AetherSDR::WdspSMeter::settleBlocks(kInputRate, kBlock);
        check(settle == 15,
              "3 tau of WDSP's 0.100 s meter average at 48 ksps / 1024-sample blocks "
              "is 15 blocks (ceil of 14.0625)");
        // The count is in blocks but the wait is a property of the tap, so it
        // has to come out as the same wall-clock time at every DDC0 rate --
        // a zoom must not settle six times longer at 48 than at 1536 ksps.
        for (const int ksps : kDdc0RatesKsps) {
            const int n = AetherSDR::WdspSMeter::settleBlocks(ksps * 1000, kBlock);
            const double seconds = static_cast<double>(n) * kBlock / (ksps * 1000.0);
            const double target = AetherSDR::WdspSMeter::kSettleTaus * AetherSDR::WdspSMeter::kAverageTauSec;
            const double oneBlock = static_cast<double>(kBlock) / (ksps * 1000.0);
            check(seconds >= target && seconds - target <= oneBlock,
                  "the settle rounds up to the same 0.3 s at every DDC0 rate");
        }
        check(AetherSDR::WdspSMeter::settleBlocks(0, 0) == 1,
              "an unconfigured rate still swallows one block rather than dividing by zero");

        AnanRxDsp dsp;
        AnanRxDsp::Config cfg;
        cfg.inputSampleRateHz = kInputRate;
        cfg.audioSampleRateHz = kAudioRate;
        cfg.dspBlockSize = kBlock;
        cfg.panPoints = 256;
        // WDSP taps this meter immediately AFTER the main bandpass -- upstream
        // RXA.c:647-648 runs xnbp(nbp0) and then xmeter(smeter) on the same
        // midbuff -- so the tone has to be inside the passband or the meter
        // reads the floor and the group proves nothing. Group 2's arrangement,
        // already established there: a +800 Hz WIRE tone lands in an LSB
        // channel's negative passband.
        cfg.mode = WdspChannel::Mode::Lsb;
        cfg.filterLowHz = -9000.0;
        cfg.filterHighHz = -100.0;
        cfg.agcMode = 0;                    // AGC off: the level is the signal's
        cfg.maximumAgcGainDb = 40.0;
        cfg.blockForOutput = true;          // deterministic for an offline burst feed
        std::string err;
        check(dsp.configure(cfg, &err), err.empty() ? "AnanRxDsp configures" : err.c_str());

        std::vector<float> levels;
        const auto conn = QObject::connect(&dsp, &AnanRxDsp::meterUpdate,
                                           [&](float dbfs) { levels.push_back(dbfs); });

        double phase = 0.0;
        const double dp = 2.0 * kPi * 800.0 / kInputRate;
        const auto feed = [&](int blocks) {
            for (int b = 0; b < blocks; ++b) {
                std::vector<std::complex<float>> iq(kBlock);
                for (int k = 0; k < kBlock; ++k) {
                    iq[static_cast<std::size_t>(k)] = {
                        static_cast<float>(0.25 * std::cos(phase)),
                        static_cast<float>(0.25 * std::sin(phase))
                    };
                    phase += dp;
                    if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
                }
                dsp.processIqBlock(iq);
            }
        };

        // A channel is born with its average at zero, so the same window
        // applies from the other end -- no mute involved.
        feed(settle);
        check(levels.empty(),
              "a freshly installed channel publishes nothing until its meter has settled");

        feed(60);
        check(!levels.empty(), "a steady tone is published once the meter has settled");
        const float preMute = levels.empty() ? -400.0f : levels.back();
        check(preMute > -100.0f, "the settled reading is a real level, not the meter floor");

        // The mute itself: the positive control above ran through this same
        // connection, so "no emissions" here can only be the guard.
        levels.clear();
        dsp.setAudioMuted(true);
        feed(12);                           // 250 ms, the rate change's settle window
        check(levels.empty(),
              "nothing is published while the channel is clocked with our own silence");

        // The edge this group exists for. Cleared first so this check cannot
        // inherit a failure of the muted-silence check above.
        levels.clear();
        dsp.setAudioMuted(false);
        feed(settle);
        check(levels.empty(),
              "and nothing is published while the meter's average still carries it");

        feed(40);
        check(!levels.empty(), "readings resume after the settle window");
        const float first = levels.empty() ? -400.0f : levels.front();
        std::fprintf(stderr, "S-meter: %.1f dBFS before the mute, %.1f dBFS on the "
                             "first reading after it\n",
                     static_cast<double>(preMute), static_cast<double>(first));
        check(std::fabs(first - preMute) < 3.0f,
              "the first reading after the mute is within 3 dB of the pre-mute level -- "
              "the needle holds through a zoom instead of diving at the end of it");

        QObject::disconnect(conn);
    }

    if (g_failures == 0)
        std::fprintf(stderr, "anan_rxdsp_handedness_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
