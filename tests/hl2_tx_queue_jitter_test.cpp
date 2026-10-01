// #6052: a transmit block that arrives late must not become keyed zeros in
// mid-over. Whole Hl2TxDsp blocks are queued at the nominal rate, each late by
// a bounded amount, and the EP2 packets MetisClient builds are read back. Time
// is virtual, counted in EP2 samples (one buildNextControlPacket() per packet).
// The lateness schedules use the gap lengths measured on hardware (hl2-lab d167).

#include "core/backends/hl2/Hl2TxDsp.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"
#include "TxTestAuthority.h"

#include <QCoreApplication>
#include <QLoggingCategory>

#include <algorithm>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    // The EP2 rate is MetisClient's own constant, read rather than retyped.
    static constexpr int ep2AudioRateHz() { return MetisClient::kEp2AudioRateHz; }
};
}

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

namespace {

constexpr int kRate = MetisClientTestAccess::ep2AudioRateHz();
constexpr int kPacket = kTxSamplesPerPacket;

// One Hl2TxDsp block as the queue receives it, in EP2 samples. Derived from
// the real Config so that a change of block size or rate moves this test.
int blockSamples()
{
    const Hl2TxDsp::Config chain{};
    return chain.dspBlockSize * (kRate / chain.inputSampleRateHz);
}

int msToSamples(double ms) { return static_cast<int>(ms * kRate / 1000.0 + 0.5); }
double samplesToMs(long long samples) { return 1000.0 * static_cast<double>(samples) / kRate; }

// Sample number `index` of the over, as the producer queues it. I carries a
// counter that survives the 16-bit conversion exactly, so the wire can be
// checked for a lost, repeated or reordered sample. Q is a non-zero constant,
// so a sample of exact zero on the wire is never one of the producer's.
constexpr int kCounterPeriod = 30000;
int counterOf(long long index) { return static_cast<int>(index % kCounterPeriod) + 1; }
std::complex<float> producerSample(long long index)
{
    return {(static_cast<float>(counterOf(index)) + 0.5f) / 32767.0f, -0.25f};
}

bool anyFrameKeyed(const std::array<std::uint8_t, kUsbPacketSize>& pkt)
{
    return ((pkt[8 + 3] | pkt[8 + kFrameSize + 3]) & kC0MoxBit) != 0;
}

// The 126 I/Q pairs of one EP2 packet, as signed 16-bit values off the wire.
struct WireSample { int i; int q; };
std::vector<WireSample> decodeIq(const std::array<std::uint8_t, kUsbPacketSize>& pkt)
{
    std::vector<WireSample> out;
    out.reserve(static_cast<std::size_t>(kPacket));
    const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
    for (const std::size_t fs : frameStarts) {
        const std::uint8_t* pay = pkt.data() + fs + 8;
        for (std::size_t k = 0; k + kTxSampleBytes <= kFramePayload; k += kTxSampleBytes) {
            const auto word = [&](std::size_t at) {
                return static_cast<int>(static_cast<std::int16_t>(
                    static_cast<std::uint16_t>((pay[at] << 8) | pay[at + 1])));
            };
            out.push_back({word(k + 4), word(k + 6)});
        }
    }
    return out;
}

struct OverResult {
    int gaps = 0;                 // runs of keyed zeros BETWEEN real samples
    long long gapSamples = 0;     // their total length
    long long longestGap = 0;
    long long firstArrival = -1;  // when the first block was queued
    long long firstReal = -1;     // wire position of the first real sample
    bool everyPacketKeyed = true;
    bool sequenceIntact = true;   // every sample once, in order
    long long realSamples = 0;
};

// One over. `lateness[j]` is how late block j is, in samples, against a
// producer that delivers one block per block period starting one block after
// the keyed edge (Hl2TxDsp has to fill a block before it can emit one).
OverResult runOver(const std::vector<int>& lateness)
{
    const int block = blockSamples();
    const auto blocks = static_cast<long long>(lateness.size());

    TxTestAuthority tx;
    MetisClient client;
    client.enableTransmit(true);
    client.setMox(true, tx.operation);

    OverResult r;
    long long nextBlock = 0;
    long long zeroRun = 0;        // zeros seen since the last real sample
    // Every block delivered, then enough ticks to drain whatever depth the
    // queue was holding. Trailing zeros after the last real sample are the end
    // of the over and are not counted: a gap is closed by a real sample.
    const long long total = blocks * block;
    for (long long tick = 0; r.realSamples < total && tick < 4 * total / kPacket; ++tick) {
        const long long now = tick * kPacket;
        while (nextBlock < blocks
               && (nextBlock + 1) * block + lateness[static_cast<std::size_t>(nextBlock)] <= now) {
            std::vector<std::complex<float>> iq(static_cast<std::size_t>(block));
            for (int n = 0; n < block; ++n)
                iq[static_cast<std::size_t>(n)] = producerSample(nextBlock * block + n);
            client.queueTxIq(iq, tx.context);
            if (r.firstArrival < 0)
                r.firstArrival = now;
            ++nextBlock;
        }
        const auto pkt = client.buildNextControlPacket();
        r.everyPacketKeyed = r.everyPacketKeyed && anyFrameKeyed(pkt);
        const auto wire = decodeIq(pkt);
        for (std::size_t s = 0; s < wire.size(); ++s) {
            if (wire[s].i == 0 && wire[s].q == 0) {
                ++zeroRun;
                continue;
            }
            if (r.firstReal < 0) {
                r.firstReal = now + static_cast<long long>(s);
            } else if (zeroRun > 0) {
                ++r.gaps;
                r.gapSamples += zeroRun;
                r.longestGap = std::max(r.longestGap, zeroRun);
            }
            zeroRun = 0;
            if (wire[s].i != counterOf(r.realSamples))
                r.sequenceIntact = false;
            ++r.realSamples;
        }
    }
    return r;
}

// Blocks in a 3 s over, the length d167 used.
int blocksInOver() { return 3 * kRate / blockSamples(); }

}   // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // The starvation log is one long line per episode and this test makes
    // dozens of episodes on purpose. The wire is what is read here.
    QLoggingCategory::setFilterRules(QStringLiteral("aether.hl2.tx.debug=false"));
    const int block = blockSamples();
    check(block == 1024, "one Hl2TxDsp block reaches the queue as 1024 EP2 samples (21.33 ms)");

    // ---- A producer that is never late ----
    // An unprimed queue is left 16 samples short of a packet when the next
    // block is due, so even this gets one sub-ms gap. The control here is the
    // sample sequence: every sample once, in order.
    {
        const OverResult r = runOver(std::vector<int>(static_cast<std::size_t>(blocksInOver()), 0));
        std::fprintf(stderr,
            "PROBE  on time: %d gap(s), %.2f ms of keyed zeros; first block queued at %.2f ms,"
            " first sample on the wire at %.2f ms (held %.2f ms)\n",
            r.gaps, samplesToMs(r.gapSamples), samplesToMs(r.firstArrival),
            samplesToMs(r.firstReal), samplesToMs(r.firstReal - r.firstArrival));
        check(r.gaps == 0, "an on-time producer has no mid-over gap");
        check(r.sequenceIntact && r.realSamples == static_cast<long long>(blocksInOver()) * block,
              "CONTROL: every sample of an on-time over reaches the wire once, in order");
        check(r.everyPacketKeyed, "CONTROL: the over is keyed from its first packet");
    }

    // ---- THE MEASURED OVERS: d167 leg C, ten 3 s overs ----
    //
    // Each row is the mid-over zero runs of one hardware over, in ms. Each run
    // becomes a STEP in the producer's lateness, held for the rest of the over,
    // from the fourth block on every second block. The positions are chosen;
    // only the lengths are measured.
    const std::vector<std::vector<double>> kMeasuredOversMs = {
        {1.96, 8.67}, {0.38, 2.67, 2.29}, {2.00}, {1.29, 1.62, 8.62}, {4.96},          // 384 kHz
        {5.54, 4.25}, {2.58, 3.92, 4.25}, {5.54, 10.17, 4.58, 3.29, 1.62, 0.67},
        {27.54}, {13.75, 2.29, 2.29, 0.58, 1.71, 1.62},                                // 48 kHz
    };
    {
        int oversWithGaps = 0;
        int gaps = 0;
        long long gapSamples = 0;
        long long longest = 0;
        long long worstHeld = 0;
        bool intact = true;
        bool keyed = true;
        for (const auto& over : kMeasuredOversMs) {
            std::vector<int> lateness(static_cast<std::size_t>(blocksInOver()), 0);
            int late = 0;
            std::size_t step = 0;
            for (std::size_t j = 3; j < lateness.size(); ++j) {
                if (step < over.size() && (j - 3) % 2 == 0)
                    late += msToSamples(over[step++]);
                lateness[j] = late;
            }
            const OverResult r = runOver(lateness);
            oversWithGaps += r.gaps > 0 ? 1 : 0;
            gaps += r.gaps;
            gapSamples += r.gapSamples;
            longest = std::max(longest, r.longestGap);
            worstHeld = std::max(worstHeld, r.firstReal - r.firstArrival);
            intact = intact && r.sequenceIntact
                     && r.realSamples == static_cast<long long>(blocksInOver()) * block;
            keyed = keyed && r.everyPacketKeyed;
        }
        std::fprintf(stderr,
            "PROBE  measured lateness (d167 leg C, 10 overs): %d gap(s) in %d of 10 overs,"
            " %.2f ms of keyed zeros in total, longest %.2f ms; first sample held %.2f ms"
            " at most\n",
            gaps, oversWithGaps, samplesToMs(gapSamples), samplesToMs(longest),
            samplesToMs(worstHeld));
        check(gaps == 0,
              "the lateness measured on hardware (d167 leg C, up to 27.5 ms in one over) "
              "puts NO keyed zeros in mid-over");
        check(intact, "late blocks are delayed, never lost, repeated or reordered");
        check(keyed, "MOX is on the first packet of the over and every one after it");
        // WHAT THE DEPTH MAY COST. Two blocks plus the packet it is released
        // on: that is the latency #6052 puts to the maintainer, and a deeper
        // queue is a different proposal.
        check(worstHeld <= 2 * block + kPacket,
              "the first sample waits in the queue for at most two blocks and one packet");
    }

    // ---- BOUNDED JITTER: every block late by 1-25 ms, independently ----
    //
    // The same requirement against a producer that is late all the time and
    // not only at a few steps. A fixed seed, so the schedule is identical on
    // every run.
    {
        std::uint32_t lcg = 0x6052u;
        std::vector<int> lateness(static_cast<std::size_t>(blocksInOver()), 0);
        for (std::size_t j = 0; j < lateness.size(); ++j) {
            lcg = lcg * 1664525u + 1013904223u;
            lateness[j] = msToSamples(1.0 + 24.0 * static_cast<double>(lcg >> 8)
                                                / static_cast<double>(1u << 24));
        }
        const OverResult r = runOver(lateness);
        std::fprintf(stderr,
            "PROBE  jitter 1-25 ms on every block: %d gap(s), %.2f ms of keyed zeros,"
            " longest %.2f ms; first sample held %.2f ms\n",
            r.gaps, samplesToMs(r.gapSamples), samplesToMs(r.longestGap),
            samplesToMs(r.firstReal - r.firstArrival));
        check(r.gaps == 0, "a producer whose every block is 1-25 ms late puts NO keyed zeros "
                           "in mid-over");
        check(r.sequenceIntact && r.realSamples == static_cast<long long>(blocksInOver()) * block,
              "and every sample still reaches the wire once, in order");
    }

    // ---- POSITIVE CONTROL: the gap detector still sees a gap ----
    //
    // One block 100 ms late is more than any queue depth proposed in #6052
    // (two blocks at most). If this read no gap, the zeros above would mean a
    // blind detector.
    {
        std::vector<int> lateness(static_cast<std::size_t>(blocksInOver()), 0);
        for (std::size_t j = 20; j < lateness.size(); ++j)
            lateness[j] = msToSamples(100.0);
        const OverResult r = runOver(lateness);
        std::fprintf(stderr,
            "PROBE  one block 100 ms late: %d gap(s), %.2f ms of keyed zeros, longest %.2f ms\n",
            r.gaps, samplesToMs(r.gapSamples), samplesToMs(r.longestGap));
        check(r.gaps >= 1 && r.longestGap >= msToSamples(100.0) - 2 * block - kPacket
                  && r.longestGap <= msToSamples(100.0) + kPacket,
              "POSITIVE CONTROL: a block later than the queue is deep is a gap, as long as "
              "the lateness less whatever depth the queue had");
        check(r.sequenceIntact, "and even then nothing is lost");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_tx_queue_jitter_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
