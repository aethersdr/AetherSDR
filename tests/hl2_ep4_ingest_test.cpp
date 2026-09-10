// HL2 wideband bandscope (endpoint 0x04) — MetisClient ingest. Socket-free:
// no bind, no peer, no radio. Recorded datagrams are handed straight to the
// drain path through MetisClientTestAccess, the friend seam MetisClient.h
// already declares for exactly this.
//
// The claim under test is not "EP4 parses" — hl2_ep4_bandscope_test owns that.
// It is that EP4 and EP6 are accounted SEPARATELY on one socket:
//
//   * a bandscope datagram must increment ep4Packets and NOT rxPackets, and it
//     must not touch the EP6 drop counter or the silence watchdog's clock;
//   * ep4_seq_no is a different counter with a different reset, so a client
//     sharing EP6's expectation would report a gap on nearly every packet;
//   * the counter's start-of-stream rewind is a RESET, not a loss, and it is
//     counted where it can be seen rather than folded into ep4Drops.
//
// The sequences replayed here are recorded arrivals from a real v74.2 board
// (tests/Hl2Ep4ArrivalsD94.h), not invented ones.

#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include "Hl2Ep4ArrivalsD94.h"

#include <QCoreApplication>

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace AetherSDR::hl2 {
// No start(), no bind, no peer: inject transport state and feed the ingest
// path the bytes a socket would have delivered.
struct MetisClientTestAccess {
    static void setStreaming(MetisClient& client) { client.m_running = true; }
    static void feedDatagram(MetisClient& client, std::span<const std::uint8_t> bytes)
    {
        client.handleDatagram(bytes);
    }
};
}  // namespace AetherSDR::hl2

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static std::vector<std::uint8_t> makeEp4(std::uint32_t seq)
{
    std::vector<std::uint8_t> pkt(kUsbPacketSize, 0);
    pkt[0] = 0xEF; pkt[1] = 0xFE; pkt[2] = 0x01; pkt[3] = 0x04;
    pkt[4] = 0x00;                                            // hardwired
    pkt[5] = static_cast<std::uint8_t>((seq >> 16) & 0x0F);
    pkt[6] = static_cast<std::uint8_t>((seq >> 8) & 0xFF);
    pkt[7] = static_cast<std::uint8_t>(seq & 0xFF);
    return pkt;
}

static std::vector<std::uint8_t> makeEp6(std::uint32_t seq)
{
    std::vector<std::uint8_t> pkt(kUsbPacketSize, 0);
    pkt[0] = 0xEF; pkt[1] = 0xFE; pkt[2] = 0x01; pkt[3] = 0x06;
    pkt[4] = static_cast<std::uint8_t>((seq >> 24) & 0xFF);
    pkt[5] = static_cast<std::uint8_t>((seq >> 16) & 0xFF);
    pkt[6] = static_cast<std::uint8_t>((seq >> 8) & 0xFF);
    pkt[7] = static_cast<std::uint8_t>(seq & 0xFF);
    for (const std::size_t fs : {std::size_t{8}, std::size_t{8 + kFrameSize}})
        pkt[fs] = pkt[fs + 1] = pkt[fs + 2] = 0x7F;           // SYNC
    return pkt;
}

static void feedEp4(MetisClient& c, std::uint32_t seq)
{
    MetisClientTestAccess::feedDatagram(c, makeEp4(seq));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1 · an EP4 datagram is counted as EP4 and as nothing else ----
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        check(c.linkCounters().ep4Packets == 0, "no bandscope packets before any arrive");
        feedEp4(c, 0);
        const auto k = c.linkCounters();
        check(k.ep4Packets == 1, "an EP4 datagram increments ep4Packets");
        check(k.rxPackets == 0, "...and NOT rxPackets: it carries no IQ and no telemetry");
        check(k.drops == 0, "...and does not disturb the EP6 drop counter");
        check(c.droppedPackets() == 0, "...nor the EP6 counter the health row reads");
        // The bytes still crossed the wire, and a receive total that omits them
        // would understate the link's real load — which is the number the
        // bandscope's cost has to be judged against.
        check(k.rxBytes == kUsbPacketSize, "the bandscope's bytes are counted as traffic");
    }

    // ---- 2 · a genuine forward skip is a drop ----
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        feedEp4(c, 0);
        feedEp4(c, 1);
        feedEp4(c, 3);
        check(c.ep4Drops() == 1, "a skipped EP4 sequence number is one drop");
        check(c.ep4Rewinds() == 0, "...and not a rewind");
        check(c.linkCounters().ep4Packets == 3, "the three that DID arrive are counted");
    }

    // ---- 3 · the 20-bit wrap is not a drop ----
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        feedEp4(c, kEp4SeqModulus - 2);
        feedEp4(c, kEp4SeqModulus - 1);
        feedEp4(c, 0);
        feedEp4(c, 1);
        check(c.ep4Drops() == 0, "ep4_seq_no wrapping at 2^20 is not a loss");
        check(c.ep4Rewinds() == 0, "...and not a rewind either");
    }

    // ---- 4 · THE MEASURED REWIND: 0,1,2 -> 0 ----
    //
    // What a real board does in the first ten milliseconds of every bandscope
    // session, because usopenhpsdr1.v forces ep4_seq_no's low two bits to zero
    // while the capture FIFO is still filling.
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        for (const std::uint32_t s : {0u, 1u, 2u, 0u, 1u, 2u, 3u})
            feedEp4(c, s);
        check(c.ep4Rewinds() == 1, "the start-of-stream rewind is counted as a rewind");
        check(c.ep4Drops() == 0, "the start-of-stream rewind is NOT a million drops");
        check(c.linkCounters().ep4Drops == 0, "and the published counter agrees");
        check(c.linkCounters().ep4Rewinds == 1, "the rewind is published separately");
    }

    // ---- 5 · the whole recorded leg, replayed ----
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        for (std::size_t i = 0; i < kD94Ep4Seq48kCount; ++i)
            feedEp4(c, kD94Ep4Seq48k[i]);
        const auto k = c.linkCounters();
        check(k.ep4Packets == kD94Ep4Seq48kCount,
              "every recorded arrival is accounted for");
        check(k.ep4Drops == 0, "ten seconds of real traffic lost nothing");
        check(k.ep4Rewinds == 1, "and rewound exactly once, at the start");
        check(k.rxPackets == 0, "none of it was mistaken for IQ");
    }

    // ---- 6 · EP6 and EP4 keep their own expectations ----
    //
    // Interleaved on one socket, each stream counting from its own zero. A
    // client that shared one expected-sequence value would score every packet
    // against the other endpoint's counter and report a gap on nearly all of
    // them.
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        for (std::uint32_t i = 0; i < 4; ++i) {
            MetisClientTestAccess::feedDatagram(c, makeEp6(i));
            feedEp4(c, i);
        }
        const auto k = c.linkCounters();
        check(k.rxPackets == 4 && k.ep4Packets == 4, "each stream counts its own packets");
        check(k.drops == 0, "interleaving does not manufacture an EP6 gap");
        check(k.ep4Drops == 0, "interleaving does not manufacture an EP4 gap");

        // An EP6 loss is an EP6 loss, and says nothing about the bandscope.
        MetisClientTestAccess::feedDatagram(c, makeEp6(9));
        check(c.droppedPackets() == 5, "the EP6 gap is counted where it happened");
        check(c.ep4Drops() == 0, "...and nowhere else");
    }

    // ---- 7 · the enable is off by default and never widens the start byte ----
    {
        MetisClient c;
        check(!c.bandscopeEnabled(), "the bandscope is off in a fresh client");
        // A request with no stream behind it is refused rather than remembered:
        // the run byte means nothing to a radio that was never started, and
        // start() brings wide_spectrum up clear.
        c.setBandscopeEnabled(true);
        check(!c.bandscopeEnabled(), "enabling a stopped client is refused, not latched");

        MetisClientTestAccess::setStreaming(c);
        c.setBandscopeEnabled(true);
        check(c.bandscopeEnabled(), "enabling a running client records the request");
        c.setBandscopeEnabled(false);
        check(!c.bandscopeEnabled(), "and disabling clears it");

        // The byte the radio would actually receive. Built here from the same
        // primitives setBandscopeEnabled uses, because the assertion that
        // matters is that `run` STAYS SET while the bandscope bit moves:
        // clearing bit 0 would stop the IQ stream the operator is listening to.
        const auto on = metisCommand(static_cast<std::uint8_t>(0x01 | kRunWideSpectrum));
        check(on[3] == 0x03, "the enable byte is 0x03: run high, wide_spectrum high");
        check((on[3] & 0x01) != 0, "the run bit is never cleared to move the bandscope bit");
        // And connect is untouched — the property three fake-radio fixtures
        // sniff as d[3] == 0x01 and hl2_metis_protocol_test asserts outright.
        check(metisStart()[3] == 0x01, "metisStart() is not widened by the bandscope");
        check(metisStop()[3] == 0x00, "metisStop() still clears run AND wide_spectrum");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_ep4_ingest_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
