// Hl2TxDsp fed as capture devices group their deliveries (one, two or three
// DSP blocks at once) at the correct average rate: no exchange may underrun,
// and the tone must reach the wire whole and phase-continuous. A wall-clock
// case whose worker stalls for a block period is load, not the defect: exit 77.
// Mechanism: third_party/wdsp/AETHERSDR-PATCHES.md, patch 15.

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
static int g_inconclusive = 0;
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
    unsigned long long stallDrops = 0;
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
    r.stallDrops = tx.modulatorStallDrops();
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
    // Splices. The tone is a 1 kHz complex exponential on the wire, so
    // iq[k+1] * conj(iq[k]) has a constant angle (the median; its sign is the
    // wire's handedness). A block read out of order, repeated or torn breaks it
    // at the block boundary with the envelope flat and the fault counter silent.
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
                 "%s: chunk %zu -> blocks %llu, faults %llu (stall drops %llu), "
                 "iq %zu / %zu expected, holes %d (longest %.1f ms), splices %d\n",
                 name, chunk, r.blocks, r.faults, r.stallDrops, r.iqSamples, r.expectedIq,
                 r.holes, r.longestHoleMs, r.splices);
    const std::string p(name);
    // Load-independent: the gate must never let an exchange underrun.
    check(r.faults == r.stallDrops,
          (p + ": no exchange underran (every fault is a stall drop)").c_str());
    if (r.stallDrops > 0) {
        // The worker was held for a whole block period: load, not the defect.
        // Bounded, so a gate that drops everything cannot hide as "load".
        check(r.stallDrops * 10 <= r.blocks,
              (p + ": stall drops are rare enough to be load").c_str());
        std::fprintf(stderr, "INCONCLUSIVE: %s: %llu block(s) dropped on a stalled "
                             "worker; the machine is too loaded to judge the rest\n",
                     name, r.stallDrops);
        ++g_inconclusive;
        return;
    }
    check(r.iqSamples == r.expectedIq, (p + ": no IQ block lost").c_str());
    check(r.holes == 0, (p + ": no hole in the modulated envelope").c_str());
    check(r.splices == 0, (p + ": the modulated tone is phase-continuous").c_str());
}

// The stall path, driven through the seam rather than by load.
struct StallRig {
    TxTestAuthority authority;
    Hl2TxDsp tx;
    Hl2TxDsp::Config cfg;
    bool stalled = false;
    std::size_t iq = 0;
    std::size_t outBlock = 0;
    std::size_t block = 0;
    std::size_t n = 0;
    QElapsedTimer sinceDelivery;

    bool open()
    {
        cfg.alcEnabled = false;
        std::string err;
        if (!tx.configure(cfg, &err)) {
            std::fprintf(stderr, "FAIL: configure: %s\n", err.c_str());
            ++g_failures;
            return false;
        }
        block = static_cast<std::size_t>(cfg.dspBlockSize);
        outBlock = block * static_cast<std::size_t>(cfg.outputSampleRateHz / cfg.inputSampleRateHz);
        QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                         [this](const std::vector<std::complex<float>>& v,
                                const TxCoordinator::Context&) { iq += v.size(); });
        tx.setOutputReadyProbeForTest([this](bool) { return !stalled; });
        return true;
    }
    // One DSP block of tone, delivered at once.
    void deliverBlock()
    {
        std::vector<float> audio(block);
        for (std::size_t k = 0; k < block; ++k, ++n)
            audio[k] = static_cast<float>(0.5 * std::sin(2.0 * M_PI * 1000.0 * n / cfg.inputSampleRateHz));
        sinceDelivery.start();
        tx.processAudioBlock(audio, TxAudioSource::Microphone, authority.context);
    }
    std::size_t blocksOut() const { return outBlock ? iq / outBlock : 0; }
    // Called as the stall is lifted. The last block's deadline (one block
    // period) passed while it was held, so a drop is correct and the "not
    // dropped" checks cannot judge: load, not the defect.
    bool heldPastDeadline(const char* name) const
    {
        const double heldMs = sinceDelivery.nsecsElapsed() / 1e6;
        const double deadlineMs = 1000.0 * cfg.dspBlockSize / cfg.inputSampleRateHz;
        if (heldMs < deadlineMs)
            return false;
        std::fprintf(stderr, "INCONCLUSIVE: %s: stall held %.1f ms, past the %.1f ms "
                             "deadline; the machine is too loaded to judge it\n",
                     name, heldMs, deadlineMs);
        ++g_inconclusive;
        return true;
    }
};

// A block arriving after the caller was idle for over a block period still
// waits for the worker: the deadline runs from when the block became due, not
// from the previous exchange. A block whose worker never answers is DROPPED.
static void stallIsTimedFromDueAndDrops()
{
    StallRig g;
    if (!g.open())
        return;
    g.deliverBlock();                  // primed credit: exchanged at once
    pumpFor(40.0);                     // idle for ~two block periods
    check(g.blocksOut() == 1, "stall: first block exchanged");

    // Worker not ready on arrival, ready 3 ms later: must wait, not bypass.
    g.stalled = true;
    g.deliverBlock();
    pumpFor(3.0);
    check(g.blocksOut() == 1,
          "stall: a block arriving after idle is not exchanged while the worker is not ready");
    if (g.heldPastDeadline("stall")) {
        g.tx.reset();
        return;
    }
    g.stalled = false;
    pumpFor(20.0);
    check(g.blocksOut() == 2, "stall: ...and is exchanged once it is");
    check(g.tx.modulatorStallDrops() == 0, "stall: ...without being dropped");

    // Worker never answers: after a block period the block is dropped, and
    // nothing is exchanged into the channel meanwhile.
    pumpFor(40.0);
    g.stalled = true;
    g.deliverBlock();
    pumpFor(60.0);
    check(g.blocksOut() == 2, "stall: a stalled worker's block is never force-exchanged");
    check(g.tx.modulatorStallDrops() == 1, "stall: ...it is dropped, once");
    check(g.tx.modulatorFaultBlocks() == 1, "stall: ...and counted as a fault");
    g.stalled = false;
    pumpFor(40.0);
    g.tx.reset();
}

// Unkey while a block is waiting, key again later: the new over's first block
// starts a fresh deadline, so it is neither exchanged into a worker that is not
// ready nor dropped against the previous over's deadline.
static void newOverStartsAFreshDeadline()
{
    StallRig g;
    if (!g.open())
        return;
    g.deliverBlock();
    pumpFor(30.0);
    g.stalled = true;
    g.deliverBlock();                  // waits: worker not ready
    pumpFor(10.0);
    if (g.heldPastDeadline("new over")) {
        g.tx.reset();
        return;
    }
    g.tx.reset();                      // unkey with it still waiting
    pumpFor(30.0);                     // > one block period since it became due

    const std::size_t before = g.blocksOut();
    g.deliverBlock();                  // first block of the next over, worker not ready
    pumpFor(3.0);
    if (g.heldPastDeadline("new over")) {
        g.tx.reset();
        return;
    }
    check(g.blocksOut() == before,
          "new over: first block is not exchanged while the worker is not ready");
    check(g.tx.modulatorStallDrops() == 0,
          "new over: first block is not dropped against the previous over's deadline");
    g.stalled = false;
    pumpFor(20.0);
    check(g.blocksOut() == before + 1, "new over: ...and is exchanged once the worker is ready");
    check(g.tx.modulatorFaultBlocks() == 0, "new over: no fault anywhere");
    g.tx.reset();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // Skip only an explicit phasing build: an undefined macro reads as 0 in #if,
    // so a bare `#if !AETHER_HL2_TX_TXA` would skip silently if the option went.
#if defined(AETHER_HL2_TX_TXA) && !AETHER_HL2_TX_TXA
    std::fprintf(stderr, "phasing modulator: synchronous, cadence cannot starve it -- skipped\n");
    return 0;
#else
    stallIsTimedFromDueAndDrops();
    newOverStartsAFreshDeadline();

    // The control: a 48 kHz device, 512 frames per 10.7 ms, reaches this stage
    // as 256 samples per delivery -- never two blocks at once.
    expectClean("48k-capture (256/delivery)", 256);
    // 44.1 kHz device, 1024 frames per 23.2 ms = ~557 samples at 24 kHz: every
    // eleventh or so delivery carries two DSP blocks. (#6004)
    expectClean("44.1k-capture (557/delivery)", 557);
    // 24 kHz device as Qt 6.8's macOS QAudioSource delivers it: 4096-byte
    // flushes of 16-bit stereo, 1024 frames = 42.7 ms, two blocks every time.
    expectClean("24k-capture as Qt delivers it (1024/delivery)", 1024);
    // 16 kHz device, 512 frames per 32 ms = 768 samples at 24 kHz: one delivery
    // in two carries two DSP blocks, whatever the poll timing.
    expectClean("16k-capture (768/delivery)", 768);

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    if (g_inconclusive) {
        std::fprintf(stderr, "SKIPPED: %d case(s) inconclusive under load\n", g_inconclusive);
        return 77;
    }
    std::fprintf(stderr, "all passed\n");
    return 0;
#endif
}
