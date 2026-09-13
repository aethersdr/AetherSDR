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
// Section 8 carries the same claim one layer up, at the IRadioBackend seam:
// the rows a health dialog reads, and the verb that is the only way to ask for
// the stream. It lives here rather than in tests/hl2_backend_test.cpp, which
// has no build target — see the banner at the top of that file.
//
// The sequences replayed here are recorded arrivals from a real v74.2 board
// (tests/Hl2Ep4ArrivalsD94.h), not invented ones.

#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/AppSettings.h"

#include "Hl2Ep4ArrivalsD94.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QString>
#include <QVariant>

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
    // Before QCoreApplication and before the first AppSettings touch: section 8
    // builds an Hl2Backend, whose construction reads the settings store.
    TestSettingsProfile profile(QStringLiteral("aether-hl2-ep4-ingest"));
    if (!profile.isValid())
        return 1;
    QCoreApplication app(argc, argv);
    AetherSDR::AppSettings::instance().load();

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

        // The byte the radio would actually receive is asserted where it is
        // COMPOSED — metisRunCommand(), in hl2_metis_protocol_test. It used to
        // be re-derived here from the same two constants, which is an assertion
        // about `0x01 | kRunWideSpectrum` and not about anything MetisClient
        // does: the implementation could drop the run bit and this would still
        // pass (PR #5650 review, blocker 1). What belongs here is the one thing
        // this target can actually see — that the function MetisClient sends
        // through keeps `run` set while the bandscope bit moves.
        check(metisRunCommand(true)[3] == 0x03 && metisRunCommand(false)[3] == 0x01,
              "the byte MetisClient sends keeps run set on both edges");
        check(metisStop()[3] == 0x00, "metisStop() still clears run AND wide_spectrum");
    }


    // ---- 8 · the backend's EP4 seam, with no link ----
    //
    // Section 7 is MetisClient's own refusal. This is the same question one
    // layer up, at IRadioBackend: what a health dialog can read, and what the
    // one verb does. Both are reachable on a default-constructed backend — no
    // socket, no peer, no discovery, no event loop — because invokeExtension
    // dispatches on the namespace and verb before it consults anything else,
    // and healthSnapshot() reads members rather than the wire.
    //
    // What is NOT here, and cannot be: the POSITIVE path. Hl2Backend gates the
    // enable on m_connected, which is set only from MetisClient::linkUp, which
    // needs a real EP6 datagram from a real peer. "Enable, and watch the health
    // row follow" is a fake-radio assertion; it is certified against hardware
    // instead, and it is not faked here.
    {
        AetherSDR::hl2::Hl2Backend backend;

        const auto snap = backend.healthSnapshot();
        const auto has = [&snap](const char* key) {
            return snap.values.contains(QString::fromLatin1(key));
        };
        // Reported WITHOUT being asked for, and reported off. An absent row
        // would leave "is this costing me link budget?" unanswered rather than
        // answered "no", which is the answer a reader of that dialog needs
        // first.
        check(has("bandscopeEnabled")
                  && !snap.values.value(QStringLiteral("bandscopeEnabled")).toBool(),
              "the bandscope is reported, and reported OFF, before anything asks");
        check(snap.values.value(QStringLiteral("ep4Packets")).toULongLong() == 0u,
              "no EP4 packets are claimed while it is off");
        // A row of its OWN, not folded into ep4Drops: exactly one rewind is
        // expected per stream start and none after, so a second one is an
        // anomaly that a counter meant to read zero would hide.
        check(has("ep4Drops") && has("ep4Rewinds"),
              "drops and rewinds are separate rows");
        check(has("bandscopeBlocks") && has("bandscopeTimeouts"),
              "the gate's own health is reported too");
        // The headroom rows are ABSENT until a block has arrived — the
        // "absent means not reported" contract doing the work no default could,
        // since 0.00 dBFS would read as a hard clip rather than as "never
        // looked at".
        check(!has("adcPeakDbfs") && !has("adcRmsDbfs") && !has("adcCrestDb"),
              "the headroom rows are ABSENT, not zero, before any block");

        // The verb. Counted rather than spied so this target needs no Qt6::Test.
        int results = 0;
        int errors = 0;
        quint64 lastId = 0;
        QVariant lastPayload;
        QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionResult, &backend,
                         [&](quint64 id, const QVariant& payload) {
            ++results; lastId = id; lastPayload = payload;
        });
        QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionError, &backend,
                         [&](quint64, const QString&) { ++errors; });

        backend.invokeExtension(QStringLiteral("hl2"),
                                QStringLiteral("bandscope.enable"), 43, QVariant(true));
        check(errors == 0, "bandscope.enable is an implemented verb, not the error stub");
        check(results == 1, "...it completes locally, like freqcal.set, with no round trip");
        check(lastId == 43u, "...carrying its requestId back");
        check(lastPayload.toMap().contains(QStringLiteral("enabled")),
              "...and reporting the state it applied");
        // REFUSED while disconnected, and reported as refused. MetisClient
        // ignores a run byte with no stream behind it, so echoing the request
        // back would be this side inventing a state the radio was never told
        // about. This is the assertion the connected case cannot make.
        check(!lastPayload.toMap().value(QStringLiteral("enabled")).toBool(),
              "a bandscope enable with no link is refused, not echoed");
        check(!backend.healthSnapshot().values
                   .value(QStringLiteral("bandscopeEnabled")).toBool(),
              "and the health row reports the refusal, not the request");

        // requestId 0 is the fire-and-forget form a caller uses when it wants no
        // reply. NOT "the UI uses": no UI reaches this verb at all — the
        // capabilities map lists it as caller-less and the verb's own comment in
        // Hl2Backend says so. (PR #5650 review, K5PTB.)
        backend.invokeExtension(QStringLiteral("hl2"),
                                QStringLiteral("bandscope.enable"), 0, QVariant(false));
        check(results == 1, "requestId 0 asks for no reply and gets none");
        check(errors == 0, "...and is still not the error stub");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_ep4_ingest_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
