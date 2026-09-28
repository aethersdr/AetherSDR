// Hl2TxDsp fed the way a LOW-RATE capture device actually delivers audio.
//
// THE DEFECT THIS PINS. Qt 6.8's macOS QAudioSource flushes captured audio
// in fixed 4096-byte chunks -- 1024 frames of 16-bit stereo -- whatever the
// device rate. At 48 kHz that is 21.3 ms, one of this stage's 512-sample
// (24 kHz) DSP blocks per delivery once the voice strip has resampled it. At
// 24 kHz -- a Bluetooth headset's microphone, AirPods Max -- it is 42.7 ms,
// TWO blocks per delivery, every delivery (measured on the live app, hl2-lab
// d161: 326 of 326). At 16 kHz it is 64 ms.
//
// The TXA channel is opened non-blocking (blockForOutput = false). Exchanging
// the two blocks back-to-back underruns the second one before WDSP's worker
// has produced its output, fexchange2 zeroes it and advances its read index,
// and from then on the channel's TWO-slot output ring is read out of step with
// its writer: every block boundary after that is a splice of the wrong or a
// torn block. That is the "chopped" transmit audio. It is almost silent in the
// telemetry -- the fault counter records the one underrun (2 in a whole over
// on the live app) and nothing for the ~250 splices that follow it.
//
// The cadence is not a caller fault that can be fixed upstream: the average
// rate is exactly right, only the grouping differs, and the grouping is the
// audio backend's. So the stage spaces its channel exchanges itself.
//
// Each case feeds a 1 kHz tone at the correct AVERAGE rate, grouped as that
// device's deliveries are grouped, with the event loop running between
// deliveries as it does on the I/O thread. Every DSP block must reach the wire
// (zero modulator faults, the full IQ count), the envelope must have no hole,
// and the modulated tone must be phase-continuous.

#include "core/backends/hl2/Hl2TxDsp.h"
#include "TxTestAuthority.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
    else     { std::fprintf(stderr, "ok:   %s\n", what); }
}

// Run the event loop -- not sleep -- until `ms` have elapsed. The live stage
// runs on a Qt thread whose loop keeps turning between capture deliveries.
static void pumpFor(double ms)
{
    QElapsedTimer t;
    t.start();
    while (t.nsecsElapsed() < static_cast<qint64>(ms * 1e6)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

struct Result {
    std::size_t iqSamples = 0;
    std::size_t expectedIq = 0;
    unsigned long long faults = 0;
    unsigned long long blocks = 0;
    int holes = 0;              // runs >= 1 ms with |iq| below 10 % of median
    double longestHoleMs = 0.0;
    int splices = 0;            // phase discontinuities in the modulated tone
};

// `chunk` input samples per delivery, one delivery per chunk's real duration.
static Result run(std::size_t chunk, double seconds)
{
    TxTestAuthority authority;
    Hl2TxDsp tx;
    Hl2TxDsp::Config cfg;          // shipped defaults: 24 kHz in, 48 kHz out, USB
    cfg.alcEnabled = false;
    std::string err;
    Result r;
    if (!tx.configure(cfg, &err)) {
        std::fprintf(stderr, "FAIL: configure: %s\n", err.c_str());
        ++g_failures;
        return r;
    }
    std::vector<std::complex<float>> out;
    QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                     [&](const std::vector<std::complex<float>>& iq,
                         const TxCoordinator::Context&) {
        out.insert(out.end(), iq.begin(), iq.end());
    });

    const int fs = cfg.inputSampleRateHz;
    const std::size_t total = static_cast<std::size_t>(seconds * fs);
    const double chunkMs = 1000.0 * static_cast<double>(chunk) / fs;
    std::size_t n = 0;
    QElapsedTimer clock;
    clock.start();
    int delivery = 0;
    while (n < total) {
        const std::size_t len = std::min(chunk, total - n);
        std::vector<float> audio(len);
        for (std::size_t k = 0; k < len; ++k, ++n)
            audio[k] = static_cast<float>(0.5 * std::sin(2.0 * M_PI * 1000.0 * n / fs));
        tx.processAudioBlock(audio, TxAudioSource::Microphone, authority.context);
        ++delivery;
        // Deliveries on the device's own period, measured from the start so a
        // slow turn is caught up rather than accumulated: the AVERAGE rate is
        // exactly right, which is what makes the grouping the only variable.
        const double due = delivery * chunkMs;
        const double now = clock.nsecsElapsed() / 1e6;
        if (due > now)
            pumpFor(due - now);
    }
    pumpFor(100.0);   // let anything the stage legitimately deferred come out

    r.faults = tx.modulatorFaultBlocks();
    r.blocks = tx.modulatorBlocks();
    r.iqSamples = out.size();
    const std::size_t block = static_cast<std::size_t>(cfg.dspBlockSize);
    r.expectedIq = (total / block) * block
                 * static_cast<std::size_t>(cfg.outputSampleRateHz / cfg.inputSampleRateHz);

    // Holes in the envelope. A single-sideband 1 kHz tone is a constant-
    // envelope IQ tone, so after the channel's up-slew settles |iq| is flat and
    // any run of near-zero samples is a block that did not reach the wire.
    const std::size_t settle = static_cast<std::size_t>(0.15 * cfg.outputSampleRateHz);
    if (out.size() > settle + 1000) {
        std::vector<float> mag;
        for (std::size_t k = settle; k < out.size(); ++k)
            mag.push_back(std::abs(out[k]));
        std::vector<float> sorted = mag;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        const float thr = 0.1f * sorted[sorted.size() / 2];
        const std::size_t minRun = static_cast<std::size_t>(cfg.outputSampleRateHz / 1000);
        std::size_t run = 0;
        for (float m : mag) {
            if (m < thr) {
                ++run;
            } else {
                if (run >= minRun) {
                    ++r.holes;
                    r.longestHoleMs = std::max(r.longestHoleMs,
                                               1000.0 * run / cfg.outputSampleRateHz);
                }
                run = 0;
            }
        }
    }
    // Splices. The same tone is a complex exponential at 1 kHz on the wire, so
    // iq[k+1] * conj(iq[k]) has a constant angle of 2*pi*1000/48000 (its sign is
    // the wire's handedness, so the median is the reference). A block
    // read out of order, repeated or torn -- which is what WDSP's two-slot
    // output ring hands back once a back-to-back exchange has put its reader
    // out of step with its writer -- breaks that at the block boundary while
    // leaving the envelope perfectly flat. The fault counter does not see it
    // either: it saw one underrun, and the damage is every block after it.
    if (out.size() > settle + 1000) {
        std::vector<double> dphi;
        for (std::size_t k = settle + 1; k < out.size(); ++k)
            dphi.push_back(std::arg(out[k] * std::conj(out[k - 1])));
        std::vector<double> sorted = dphi;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        const double step = sorted[sorted.size() / 2];
        const double expected = 2.0 * M_PI * 1000.0 / cfg.outputSampleRateHz;
        if (std::abs(std::abs(step) - expected) > 0.01) {
            std::fprintf(stderr, "FAIL: tone is not at 1 kHz on the wire (step %.5f rad)\n", step);
            ++g_failures;
        }
        for (double d : dphi) {
            if (std::abs(d - step) > 0.05)
                ++r.splices;
        }
    }
    return r;
}

static void expectClean(const char* name, std::size_t chunk)
{
    const Result r = run(chunk, 2.0);
    std::fprintf(stderr,
                 "%s: chunk %zu -> blocks %llu, faults %llu, iq %zu / %zu expected, "
                 "holes %d (longest %.1f ms), splices %d\n",
                 name, chunk, r.blocks, r.faults, r.iqSamples, r.expectedIq,
                 r.holes, r.longestHoleMs, r.splices);
    const std::string p(name);
    check(r.faults == 0, (p + ": every DSP block was placed (no modulator underrun)").c_str());
    check(r.iqSamples == r.expectedIq, (p + ": no IQ block lost").c_str());
    check(r.holes == 0, (p + ": no hole in the modulated envelope").c_str());
    check(r.splices == 0, (p + ": the modulated tone is phase-continuous").c_str());
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // Skip ONLY in a build that explicitly selects the phasing modulator
    // (AETHER_HL2_TX_TXA=0). A bare `#if !AETHER_HL2_TX_TXA` reads an UNDEFINED
    // macro as 0, so once #5906 removes the build option -- TXA then being the
    // only modulator, and the macro defined nowhere -- this test would skip
    // itself, report Passed, and run none of its checks.
#if defined(AETHER_HL2_TX_TXA) && !AETHER_HL2_TX_TXA
    std::fprintf(stderr, "phasing modulator: synchronous, cadence cannot starve it -- skipped\n");
    return 0;
#else
    // The control: a 48 kHz device, 512 frames per 10.7 ms, reaches this stage
    // as 256 samples per delivery -- never two blocks at once. Clean before and
    // after the fix; if this fails the machine is too loaded to judge the rest.
    expectClean("48k-capture (256/delivery)", 256);
    // 24 kHz device as Qt 6.8's macOS QAudioSource actually delivers it:
    // 4096-byte flushes of 16-bit stereo, 1024 frames = 42.7 ms. Measured on
    // the live app (hl2-lab d161): 326 of 326 deliveries were exactly this.
    expectClean("24k-capture as Qt delivers it (1024/delivery)", 1024);
    // 16 kHz device, 512 frames per 32 ms = 768 samples at 24 kHz: one delivery
    // in two carries two DSP blocks, whatever the poll timing.
    expectClean("16k-capture (768/delivery)", 768);

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all passed\n");
    return 0;
#endif
}
