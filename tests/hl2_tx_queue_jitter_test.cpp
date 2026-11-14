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
#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    // The EP2 rate is MetisClient's own constant, read rather than retyped.
    static constexpr int ep2AudioRateHz() { return MetisClient::kEp2AudioRateHz; }
    // A session without a socket: the client keys only while running.
    static void setStreaming(MetisClient& client) { client.m_running = true; }
    // The prime and the queue's own bounds, read from the production constants.
    static constexpr std::size_t primeTarget() { return MetisClient::kTxPrimeTargetSamples; }
    static constexpr std::size_t primeMargin()
    {
        return MetisClient::kTxPrimeTargetSamples - MetisClient::kTxPrimeBlockSamples;
    }
    static constexpr std::size_t trimMargin() { return MetisClient::kTxTrimMarginSamples; }
    static constexpr std::size_t queueMax() { return MetisClient::kTxQueueMax; }
    static constexpr std::uint64_t quietPackets() { return MetisClient::kUnderflowRunQuietPackets; }
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

// The largest delivery a producer makes at once, in EP2 samples (42.67 ms): a
// TCI client answering TX_CHRONO with 2048 mono samples at 48 kHz, which
// TciIoWorker::receiveBinary() reads as 2048 frames. The trim margin promises to
// hold this much; a grouping beyond it is not a producer the queue is built for.
constexpr int kLargestDeliverySamples = 2048;

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
    int skips = 0;                // forward jumps in the sample sequence (drops)
    long long skipped = 0;        // samples the jumps passed over
    std::uint64_t trimSamples = 0;
    std::uint64_t trimEvents = 0;
    std::uint64_t overflowSamples = 0;
    std::vector<std::size_t> depth;   // queue depth after each packet
    std::vector<long long> arrivalTick;   // tick each block was queued on
    std::vector<long long> gapEndTick;    // tick each mid-over gap closed on
};

// One over. `lateness[j]` is how late block j is, in samples, against a
// producer that delivers one block per block period starting one block after
// the keyed edge (Hl2TxDsp has to fill a block before it can emit one).
// `group` > 1 delivers the blocks `group` at a time: block j is due at the end
// of its group, (j / group + 1) * group * block. TCI's TX_CHRONO period is 2048
// samples, so it reaches the queue as four 512-sample blocks at once (two on
// the 1024-sample block before #7182).
long long dueOf(long long j, int block, int group)
{
    return (j / group + 1) * group * static_cast<long long>(block);
}

OverResult runOver(const std::vector<int>& lateness, int group = 1)
{
    const int block = blockSamples();
    const auto blocks = static_cast<long long>(lateness.size());

    TxTestAuthority tx;
    MetisClient client;
    MetisClientTestAccess::setStreaming(client);
    client.enableTransmit(true);
    client.setMox(true, tx.operation);

    OverResult r;
    long long nextBlock = 0;
    long long zeroRun = 0;        // zeros seen since the last real sample
    // Every block delivered, then enough ticks to drain whatever depth the
    // queue was holding. Trailing zeros after the last real sample are the end
    // of the over and are not counted: a gap is closed by a real sample.
    const long long total = blocks * block;
    long long expected = 0;       // producer index the next real sample should carry
    for (long long tick = 0;
         r.realSamples + static_cast<long long>(client.txTrimSamples() + client.txOverflowSamples()) < total
         && tick < 4 * total / kPacket;
         ++tick) {
        const long long now = tick * kPacket;
        while (nextBlock < blocks
               && dueOf(nextBlock, block, group) + lateness[static_cast<std::size_t>(nextBlock)] <= now) {
            std::vector<std::complex<float>> iq(static_cast<std::size_t>(block));
            for (int n = 0; n < block; ++n)
                iq[static_cast<std::size_t>(n)] = producerSample(nextBlock * block + n);
            client.queueTxIq(iq, tx.context);
            r.arrivalTick.push_back(tick);
            if (r.firstArrival < 0)
                r.firstArrival = now;
            ++nextBlock;
        }
        const auto pkt = client.buildNextControlPacket();
        r.everyPacketKeyed = r.everyPacketKeyed && anyFrameKeyed(pkt);
        r.depth.push_back(client.txQueueDepth());
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
                r.gapEndTick.push_back(tick);
                r.gapSamples += zeroRun;
                r.longestGap = std::max(r.longestGap, zeroRun);
            }
            zeroRun = 0;
            if (wire[s].i != counterOf(expected)) {
                r.sequenceIntact = false;
                // A drop is a forward jump; the counter is read modulo its period.
                const long long jump = (wire[s].i - counterOf(expected) + kCounterPeriod) % kCounterPeriod;
                ++r.skips;
                r.skipped += jump;
                expected += jump;
            }
            ++expected;
            ++r.realSamples;
        }
    }
    r.trimSamples = client.txTrimSamples();
    r.trimEvents = client.txTrimEvents();
    r.overflowSamples = client.txOverflowSamples();
    return r;
}

// Blocks in a 3 s over, the length d167 used.
int blocksInOver() { return 3 * kRate / blockSamples(); }

// One block of a scheduled over: when it is queued (EP2 samples since the keyed
// edge) and under which of two producer contexts of the SAME held operation.
// Producer numbering runs on across the contexts, so the wire sequence check
// reads one continuous stream and any trim shows as a skip.
struct ScheduledBlock { long long due; int context; };
// A keying event at a sample time, applied before that tick's packet.
enum class KeyEvent { Unkey, Rekey, CwDown, CwClear };
struct ScheduledKey { long long at; KeyEvent event; };
struct ScheduleResult {
    std::uint64_t trimSamples = 0;
    std::uint64_t trimEvents = 0;
    std::uint64_t overflowSamples = 0;
    int skips = 0;
    long long realSamples = 0;
    long long gapsBeforeContext1 = 0;   // keyed gaps closed before context 1's first sample
};

ScheduleResult runSchedule(const std::vector<ScheduledBlock>& schedule,
                           const std::vector<ScheduledKey>& keys = {})
{
    const int block = blockSamples();
    TxTestAuthority tx;
    // A second producer on the same operation: a source change under held MOX.
    const auto producerB = tx.coordinator.registerProducer();
    const AetherSDR::TxCoordinator::Context contexts[2] = {
        tx.context, tx.coordinator.mediaContext(producerB, tx.operation)};
    MetisClient client;
    MetisClientTestAccess::setStreaming(client);
    client.enableTransmit(true);
    client.setMox(true, tx.operation);

    ScheduleResult r;
    const auto blocks = static_cast<long long>(schedule.size());
    const long long total = blocks * block;
    long long nextBlock = 0;
    long long expected = 0;
    long long zeroRun = 0;
    bool seenReal = false;
    bool seenContext1 = false;
    long long context1First = -1;   // producer index of context 1's first sample
    std::size_t nextKey = 0;
    bool cw = false;   // CW owns the stream: its envelope is on the wire, not the producer's
    for (long long tick = 0;
         r.realSamples + static_cast<long long>(client.txTrimSamples() + client.txOverflowSamples()) < total
         && tick < 4 * (schedule.back().due + total) / kPacket;
         ++tick) {
        const long long now = tick * kPacket;
        while (nextKey < keys.size() && keys[nextKey].at <= now) {
            switch (keys[nextKey].event) {
            case KeyEvent::Unkey:   client.setMox(false, tx.operation); break;
            case KeyEvent::Rekey:   client.setMox(true, tx.operation); break;
            case KeyEvent::CwDown:  client.setCwKeyDown(true, tx.operation); cw = true; break;
            case KeyEvent::CwClear: client.clearCwKeying(); cw = false; break;
            }
            ++nextKey;
        }
        while (nextBlock < blocks && schedule[static_cast<std::size_t>(nextBlock)].due <= now) {
            const auto& b = schedule[static_cast<std::size_t>(nextBlock)];
            if (b.context == 1 && context1First < 0)
                context1First = nextBlock * block;
            std::vector<std::complex<float>> iq(static_cast<std::size_t>(block));
            for (int n = 0; n < block; ++n)
                iq[static_cast<std::size_t>(n)] = producerSample(nextBlock * block + n);
            client.queueTxIq(iq, contexts[b.context]);
            ++nextBlock;
        }
        const auto pkt = client.buildNextControlPacket();
        if (cw)
            continue;
        const auto wire = decodeIq(pkt);
        for (const WireSample& w : wire) {
            if (w.i == 0 && w.q == 0) {
                ++zeroRun;
                continue;
            }
            if (seenReal && zeroRun > 0 && !seenContext1)
                ++r.gapsBeforeContext1;
            seenReal = true;
            zeroRun = 0;
            if (w.i != counterOf(expected)) {
                const long long jump = (w.i - counterOf(expected) + kCounterPeriod) % kCounterPeriod;
                ++r.skips;
                expected += jump;
            }
            if (context1First >= 0 && expected >= context1First)
                seenContext1 = true;
            ++expected;
            ++r.realSamples;
        }
    }
    r.trimSamples = client.txTrimSamples();
    r.trimEvents = client.txTrimEvents();
    r.overflowSamples = client.txOverflowSamples();
    return r;
}

}   // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // The starvation log is one long line per episode and this test makes
    // dozens of episodes on purpose. The wire is what is read here.
    QLoggingCategory::setFilterRules(QStringLiteral("aether.hl2.tx.debug=false"));
    const int block = blockSamples();
    check(block == 512, "one Hl2TxDsp block reaches the queue as 512 EP2 samples (10.67 ms)");
    const auto margin = static_cast<long long>(MetisClientTestAccess::primeMargin());
    const auto target = static_cast<long long>(MetisClientTestAccess::primeTarget());

    // ---- A producer that is never late ----
    // An unprimed queue is left a few samples short of a packet when the next
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
        check(r.trimEvents == 0 && r.trimSamples == 0, "CONTROL: an on-time over is never trimmed");
        // THE START (#6052): from the keyed edge, one block to fill, the margin
        // to queue, then the next packet. Shorter blocks move it by one block.
        check(r.firstReal == (target + kPacket - 1) / kPacket * kPacket,
              "the first sample goes out on the first packet after one block plus the margin");
    }

    // ---- THE MEASURED OVERS: d167 leg C, ten 3 s overs ----
    //
    // Each row is the mid-over zero runs of one hardware over, in ms. Each run
    // becomes a STEP in the producer's lateness, held for the rest of the over,
    // from the first block after the prime's target on, every second block.
    // The positions are chosen; only the lengths are measured.
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
        std::uint64_t trimmed = 0;
        for (const auto& over : kMeasuredOversMs) {
            std::vector<int> lateness(static_cast<std::size_t>(blocksInOver()), 0);
            int late = 0;
            std::size_t step = 0;
            const auto first = static_cast<std::size_t>(target / block);
            for (std::size_t j = first; j < lateness.size(); ++j) {
                if (step < over.size() && (j - first) % 2 == 0)
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
            trimmed += r.trimSamples;
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
        check(trimmed == 0, "the measured lateness never trims the queue");
        // The same lateness from a producer that delivers 2 or 4 blocks at once.
        // The gap and depth requirements above are for one block per delivery;
        // what grouping must not do is trim.
        for (const int group : {2, 4}) {
            std::uint64_t groupedTrims = 0;
            for (const auto& over : kMeasuredOversMs) {
                std::vector<int> lateness(static_cast<std::size_t>(blocksInOver()), 0);
                int late = 0;
                std::size_t step = 0;
                const auto first = static_cast<std::size_t>(target / block);
                for (std::size_t j = first; j < lateness.size(); ++j) {
                    if (step < over.size() && (j - first) % 2 == 0)
                        late += msToSamples(over[step++]);
                    lateness[j] = late;
                }
                groupedTrims += runOver(lateness, group).trimEvents;
            }
            std::fprintf(stderr, "PROBE  measured lateness, %d blocks per delivery: %llu trim(s)\n",
                         group, static_cast<unsigned long long>(groupedTrims));
            check(groupedTrims == 0,
                  "the measured lateness never trims the queue when blocks arrive 2 or 4 at once");
        }
        // WHAT THE DEPTH MAY COST. The margin plus the packet it is released
        // on: that is the latency #6052 puts to the maintainer, and a deeper
        // queue is a different proposal.
        check(worstHeld <= margin + kPacket,
              "the first sample waits in the queue for at most the margin and one packet");
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
        check(r.trimSamples == 0, "and bounded jitter is never trimmed");
        for (const int group : {2, 4}) {
            const OverResult g = runOver(lateness, group);
            std::fprintf(stderr, "PROBE  jitter 1-25 ms, %d blocks per delivery: %llu trim(s)\n",
                         group, static_cast<unsigned long long>(g.trimEvents));
            check(g.trimEvents == 0 && g.trimSamples == 0,
                  "bounded jitter is never trimmed when blocks arrive 2 or 4 at once");
        }
    }

    // ---- POSITIVE CONTROL: the gap detector still sees a gap ----
    //
    // One block 100 ms late is more than any queue depth proposed in #6052
    // (a 42.67 ms margin). If this read no gap, the zeros above would mean a
    // blind detector.
    {
        std::vector<int> lateness(static_cast<std::size_t>(blocksInOver()), 0);
        for (std::size_t j = 20; j < lateness.size(); ++j)
            lateness[j] = msToSamples(100.0);
        const OverResult r = runOver(lateness);
        std::fprintf(stderr,
            "PROBE  one block 100 ms late: %d gap(s), %.2f ms of keyed zeros, longest %.2f ms\n",
            r.gaps, samplesToMs(r.gapSamples), samplesToMs(r.longestGap));
        check(r.gaps >= 1 && r.longestGap >= msToSamples(100.0) - margin - kPacket
                  && r.longestGap <= msToSamples(100.0) + kPacket,
              "POSITIVE CONTROL: a block later than the queue is deep is a gap, as long as "
              "the lateness less whatever depth the queue had");
        check(r.sequenceIntact, "and even then nothing is lost");
        check(r.trimEvents == 0,
              "a starvation followed by on-time blocks leaves no backlog, so nothing is trimmed");
    }

    // ---- A PAUSE THEN A BURST (hl2-lab d226): the backlog is trimmed once ----
    //
    // Delivery stops for `pauseMs` mid-over; every block due in that time then
    // arrives in one burst. Untrimmed, the backlog rides as latency to the end
    // of the over; the trim must bring the depth back to the prime target once
    // the starvation episode has ended, and leave it there.
    const std::size_t ceiling =
        static_cast<std::size_t>(target) + MetisClientTestAccess::trimMargin();
    const auto quiet = static_cast<long long>(MetisClientTestAccess::quietPackets());
    // `group` 4 is a 2048-sample TCI delivery on the 512-sample block; 2 is the
    // same on the 1024-sample block it replaced. The trim lands wherever the delivery cycle is
    // when the quiet period ends, so the margin must hold a whole delivery.
    for (const int group : {1, 2, 4})
    for (const double pauseMs : {190.0, 300.0}) {
        if (group * block > kLargestDeliverySamples) {
            std::fprintf(stderr, "PROBE  %d block(s) per delivery: %d samples at once is beyond any"
                                 " producer (%d), not run\n",
                         group, group * block, kLargestDeliverySamples);
            continue;
        }
        const int stalled = 40;   // the first block held back (~0.43 s into the over); a group start
        const long long resume = dueOf(stalled, block, group) + msToSamples(pauseMs);
        std::vector<int> lateness(static_cast<std::size_t>(blocksInOver()), 0);
        for (std::size_t j = stalled; j < lateness.size(); ++j) {
            const long long due = dueOf(static_cast<long long>(j), block, group);
            if (due < resume)
                lateness[j] = static_cast<int>(resume - due);
        }
        const OverResult r = runOver(lateness, group);
        const long long burstTick = r.arrivalTick[static_cast<std::size_t>(stalled)];
        const long long lastTick = r.arrivalTick.back();
        std::size_t peak = 0;
        std::size_t after = 0;
        for (long long t = burstTick; t < lastTick; ++t) {
            const std::size_t d = r.depth[static_cast<std::size_t>(t)];
            peak = std::max(peak, d);
            if (t > burstTick + quiet)
                after = std::max(after, d);
        }
        int gapsAfterBurst = 0;
        for (const long long t : r.gapEndTick)
            gapsAfterBurst += t > burstTick ? 1 : 0;
        std::fprintf(stderr,
            "PROBE  %d block(s) per delivery, %.0f ms pause then a burst: peak depth %zu (%.2f ms), depth after %lld"
            " packets at most %zu (%.2f ms); trimmed %llu sample(s) in %llu trim(s),"
            " overflow %llu, %d skip(s) of %lld sample(s), %d gap(s) after the burst\n",
            group, pauseMs, peak, samplesToMs(static_cast<long long>(peak)), quiet + 1, after,
            samplesToMs(static_cast<long long>(after)),
            static_cast<unsigned long long>(r.trimSamples),
            static_cast<unsigned long long>(r.trimEvents),
            static_cast<unsigned long long>(r.overflowSamples), r.skips, r.skipped,
            gapsAfterBurst);
        check(peak > ceiling, "CONTROL: the burst leaves a backlog above the prime target plus margin");
        check(r.gaps >= 1, "CONTROL: the pause is a keyed gap on the wire");
        check(after <= ceiling,
              "once the starvation episode ends, the depth returns to the prime target plus "
              "margin and stays there to the end of the over");
        check(r.trimEvents == 1, "the backlog is trimmed exactly once");
        check(r.trimSamples > 0
                  && static_cast<long long>(r.trimSamples + r.overflowSamples) == r.skipped,
              "every sample missing from the wire is counted, as trim or overflow");
        check(gapsAfterBurst == 0, "the trim adds no keyed silence");
        if (pauseMs < 200.0) {
            check(r.overflowSamples == 0 && r.skips == 1,
                  "a 190 ms backlog stays under the cap: the trim is the only discontinuity");
        } else {
            check(peak + kPacket >= MetisClientTestAccess::queueMax() && r.overflowSamples > 0,
                  "CONTROL: a 300 ms backlog reaches the overflow cap, which stays the bound");
        }
    }

    // ---- A SHORT PAUSE IS NOT A BACKLOG ----
    //
    // 45-70 ms is about the queue's own depth: it starves for a few ms at most,
    // and the burst refills it to no more than one delivery above the target.
    // Nothing may be trimmed, at any delivery grouping.
    {
        std::uint64_t shortTrims = 0;
        for (const int group : {1, 2, 4}) {
            for (const double pauseMs : {45.0, 55.0, 60.0, 70.0}) {
                const int stalled = 40;
                const long long resume = dueOf(stalled, block, group) + msToSamples(pauseMs);
                std::vector<int> lateness(static_cast<std::size_t>(blocksInOver()), 0);
                for (std::size_t j = stalled; j < lateness.size(); ++j) {
                    const long long due = dueOf(static_cast<long long>(j), block, group);
                    if (due < resume)
                        lateness[j] = static_cast<int>(resume - due);
                }
                const OverResult r = runOver(lateness, group);
                shortTrims += r.trimEvents;
                if (r.trimEvents != 0)
                    std::fprintf(stderr, "PROBE  %d block(s) per delivery, %.0f ms pause: %llu trim(s)\n",
                                 group, pauseMs, static_cast<unsigned long long>(r.trimEvents));
            }
        }
        std::fprintf(stderr, "PROBE  45-70 ms pauses at 1, 2 and 4 blocks per delivery: %llu trim(s)\n",
                     static_cast<unsigned long long>(shortTrims));
        check(shortTrims == 0, "a 45-70 ms pause is never trimmed, at 1, 2 or 4 blocks per delivery");
    }

    // ---- FRONT-LOADED AUDIO AT THE START OF A PRIMED STREAM IS NEVER CUT ----
    //
    // The trim is for a backlog left by a MID-OVER starvation. Audio a producer
    // front-loads when its stream starts is not one: neither at key-down nor
    // after a context change under held MOX (a source change, mic to TCI/DAX),
    // which abandons the queue and primes it again. The front-load is sized to
    // sit above the trim threshold, or this would pass with the trim armed.
    {
        const auto frontLoad = static_cast<int>((static_cast<std::size_t>(target) + MetisClientTestAccess::trimMargin())
                                         / static_cast<std::size_t>(block)) + 2;
        const long long period = block;
        // (i) Context A plays 20 blocks on time, then stops for 200 ms (the queue
        // starves mid-over); context B, a second producer on the same operation,
        // arrives with frontLoad blocks at once and then runs on time.
        {
            std::vector<ScheduledBlock> schedule;
            for (int j = 0; j < 20; ++j)
                schedule.push_back({(j + 1) * period, 0});
            const long long bStart = 20 * period + msToSamples(200.0);
            for (int j = 0; j < frontLoad; ++j)
                schedule.push_back({bStart, 1});
            for (int m = 1; m <= 60; ++m)
                schedule.push_back({bStart + m * period, 1});
            const ScheduleResult r = runSchedule(schedule);
            std::fprintf(stderr,
                "PROBE  context change under held MOX after a 200 ms starvation, %d blocks"
                " front-loaded: trimmed %llu sample(s) in %llu trim(s), overflow %llu, %d skip(s),"
                " %lld keyed gap(s) before B\n",
                frontLoad, static_cast<unsigned long long>(r.trimSamples),
                static_cast<unsigned long long>(r.trimEvents),
                static_cast<unsigned long long>(r.overflowSamples), r.skips, r.gapsBeforeContext1);
            check(r.gapsBeforeContext1 >= 1, "CONTROL: context A's pause starves the queue mid-over");
            check(r.trimEvents == 0 && r.trimSamples == 0,
                  "a context change under held MOX starts a new primed stream: A's starvation does not "
                  "trim B's front-loaded audio");
            check(r.overflowSamples == 0 && r.skips == 0,
                  "and every sample of B reaches the wire once, in order");
        }
        // (ii) A primed stream that starts on an EMPTY queue in the SAME context,
        // so no abandon follows to disarm anything: the keyed edge of a second
        // over, and voice resuming after CW under held PTT. Each is preceded by
        // ~25 ms of empty keyed packets while the prime is armed, then frontLoad
        // blocks at once and on time after. Context A plays 20 blocks first.
        for (const bool viaCw : {false, true}) {
            std::vector<ScheduledBlock> schedule;
            for (int j = 0; j < 20; ++j)
                schedule.push_back({(j + 1) * period, 0});
            // The 20 blocks have drained: held for the prime target, then played.
            const long long edge = 20 * period + static_cast<long long>(target) + period;
            std::vector<ScheduledKey> keys;
            if (viaCw) {
                keys.push_back({edge, KeyEvent::CwDown});
                keys.push_back({edge + 10 * period, KeyEvent::CwClear});
            } else {
                keys.push_back({edge, KeyEvent::Unkey});
                keys.push_back({edge + kPacket, KeyEvent::Rekey});
            }
            const long long start = keys.back().at + msToSamples(25.0);
            for (int j = 0; j < frontLoad; ++j)
                schedule.push_back({start, 0});
            for (int m = 1; m <= 60; ++m)
                schedule.push_back({start + m * period, 0});
            const ScheduleResult r = runSchedule(schedule, keys);
            std::fprintf(stderr,
                "PROBE  %s, then %d blocks front-loaded after 25 ms in the same context: trimmed"
                " %llu sample(s) in %llu trim(s)\n",
                viaCw ? "CW then voice under held PTT" : "a second keyed edge", frontLoad,
                static_cast<unsigned long long>(r.trimSamples),
                static_cast<unsigned long long>(r.trimEvents));
            check(r.trimEvents == 0 && r.trimSamples == 0,
                  viaCw ? "voice front-loaded after CW under held PTT is never cut: the empty "
                          "packets before it are the prime's, not a mid-over starvation"
                        : "front-loaded audio at a keyed edge is never cut: the key-down underflow "
                          "does not arm the trim");
        }
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_tx_queue_jitter_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
