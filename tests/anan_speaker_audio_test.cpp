// ANAN speaker stream (DDC Audio, PC -> radio) -- the SEND PLUMBING.
//
// P2Protocol's own test covers the packet encoding and AnanSpeakerPacing's law
// as arithmetic. Neither of those touches the part that actually puts audio on
// the wire: the queue, the whole-packet boundary, the sequence counter, the
// overflow policy, and the enable gate. That is what this pins, and it does so
// by reading the datagrams P2Client really sends.
//
// NO RADIO. P2Client is started against loopback, which is enough to bind its
// socket and enter the running state -- nothing replies, and nothing here needs
// a reply. A listener on kSpeakerAudioPort receives what the client sends, so
// these assertions are about bytes that crossed a socket rather than about
// internal state agreeing with itself.
//
// The drain is invoked DIRECTLY rather than waited on. Its timer cadence is not
// under test (the pacer decides the rate, not the tick), and a test that slept
// for real timers would be slow and flaky for no added coverage.

#include "core/backends/anan/P2Client.h"
#include "core/backends/anan/P2Protocol.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QUdpSocket>

#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace AetherSDR::anan {
// Same friend-struct pattern spectrum_sequence_gap_test uses for the receive
// side: reach the private drain and the private counters without widening
// P2Client's own API for a test's benefit.
struct P2ClientTestAccess {
    // Drain with the elapsed time UNDER TEST CONTROL, which is the only way any
    // assertion about burst size can be stable.
    //
    // onSpeakerDrainTick() advances the pacer from a real clock, so wall time
    // between calls silently drains the estimate -- and a test that waits on a
    // socket spends real time by definition. An earlier version of this file let
    // that happen and asserted an exact packet count afterwards: the wait for a
    // datagram that was never coming spent 250 ms, emptied the estimate, and the
    // next drain released a full target instead of the two packets expected.
    // Restarting the clock here makes the tick's own advance ~0, so `aged` is the
    // whole of the drain the pacer sees.
    static void drain(P2Client& c, double aged = 0.0)
    {
        c.m_speakerClock.restart();
        if (aged > 0.0) {
            c.m_speakerPacer.advance(aged);
        }
        c.onSpeakerDrainTick();
    }
    static std::size_t pendingSamples(const P2Client& c) { return c.m_speakerPending.size(); }
    static void feedDatagram(P2Client& c, std::span<const std::uint8_t> b, quint16 port)
    {
        c.handleDatagram(b, port);
    }
    static std::uint32_t sequence(const P2Client& c) { return c.m_speakerSequence; }
    // Drain time expressed in packets, so the tests read in the same units the
    // pacer's target does.
    static double packetSeconds(int packets)
    {
        return packets * static_cast<double>(kSpeakerFramesPerPacket)
             / static_cast<double>(kSpeakerSampleRateHz);
    }
};
}  // namespace AetherSDR::anan

using namespace AetherSDR;
using namespace AetherSDR::anan;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// One packet's worth of interleaved int16, every sample set to `value` so a
// received packet can be traced back to the block it came from.
static QByteArray blockOf(std::int16_t value, int packets = 1)
{
    std::vector<std::int16_t> v(
        static_cast<std::size_t>(kSpeakerFramesPerPacket * kSpeakerChannels * packets), value);
    return QByteArray(reinterpret_cast<const char*>(v.data()),
                      static_cast<qsizetype>(v.size() * sizeof(std::int16_t)));
}

static std::int16_t firstSampleOf(const QByteArray& datagram)
{
    const auto hi = static_cast<std::uint8_t>(datagram[4]);
    const auto lo = static_cast<std::uint8_t>(datagram[5]);
    return static_cast<std::int16_t>(static_cast<std::uint16_t>((hi << 8) | lo));
}

static std::uint32_t sequenceOf(const QByteArray& d)
{
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[0])) << 24)
         | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[1])) << 16)
         | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[2])) << 8)
         |  static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[3]));
}

// `expected` is how many datagrams the caller believes are coming. Waiting stops
// as soon as that many have arrived, so the "expected none" case does not sit out
// a full timeout -- which is both slow and, before drain() owned the clock, the
// thing that corrupted the next assertion.
static std::vector<QByteArray> collect(QUdpSocket& listener, int expected)
{
    std::vector<QByteArray> out;
    while (static_cast<int>(out.size()) < expected) {
        if (!listener.waitForReadyRead(250)) {
            break;   // nothing more is coming; the caller's count check reports it
        }
        while (listener.hasPendingDatagrams()) {
            out.push_back(listener.receiveDatagram().data());
        }
    }
    // A short sweep for anything the client sent that the caller did NOT expect:
    // without it, "releases only four" could not tell four from fourteen.
    if (listener.waitForReadyRead(60)) {
        while (listener.hasPendingDatagrams()) {
            out.push_back(listener.receiveDatagram().data());
        }
    }
    return out;
}

static P2Client::Params loopbackParams(bool speakerAudio)
{
    P2Client::Params p;
    p.host = QStringLiteral("127.0.0.1");
    p.speakerAudioEnabled = speakerAudio;
    return p;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QUdpSocket listener;
    if (!listener.bind(QHostAddress::LocalHost, kSpeakerAudioPort)) {
        // An environmental skip, not a pass: something else on this machine holds
        // the port. Said out loud, because a silent "0 checks, exit 0" is
        // indistinguishable from a suite that ran -- and 77 says the same thing to
        // CTest, which records SKIP rather than counting this as coverage that ran.
        std::fprintf(stderr,
                     "anan_speaker_audio_test: SKIP -- cannot bind 127.0.0.1:%u (%s)\n",
                     static_cast<unsigned>(kSpeakerAudioPort),
                     qPrintable(listener.errorString()));
        return 77;   // SKIP_RETURN_CODE in tests.cmake; see AGENTS.md on socket tests.
    }

    // ---- one block in, one packet out, byte-for-byte ----
    {
        P2Client client;
        check(client.start(loopbackParams(true)), "client starts against loopback");
        client.enqueueSpeakerAudio(blockOf(0x1234));
        check(P2ClientTestAccess::pendingSamples(client)
                  == static_cast<std::size_t>(kSpeakerFramesPerPacket * kSpeakerChannels),
              "one block queues exactly one packet's worth of samples");

        P2ClientTestAccess::drain(client);
        const auto got = collect(listener, 1);
        check(got.size() == 1, "one queued packet sends exactly one datagram");
        if (got.size() == 1) {
            check(got[0].size() == static_cast<qsizetype>(kSpeakerPacketBytes),
                  "the datagram is 260 bytes on the wire");
            check(sequenceOf(got[0]) == 0, "the first packet of a session is sequence 0");
            check(firstSampleOf(got[0]) == 0x1234,
                  "the sample reaches the wire big-endian and unaltered");
        }
        check(P2ClientTestAccess::pendingSamples(client) == 0,
              "a sent packet is consumed from the queue");
        client.stop();
    }

    // ---- a partial packet waits; it is never padded out and sent early ----
    {
        P2Client client;
        client.start(loopbackParams(true));
        std::vector<std::int16_t> half(kSpeakerFramesPerPacket, 7);  // half a packet
        client.enqueueSpeakerAudio(QByteArray(reinterpret_cast<const char*>(half.data()),
                                              static_cast<qsizetype>(half.size() * sizeof(std::int16_t))));
        P2ClientTestAccess::drain(client);
        check(collect(listener, 0).empty(), "half a packet sends nothing");
        check(P2ClientTestAccess::pendingSamples(client) == half.size(),
              "the partial block stays queued for the rest of itself");

        // The other half completes it, and the two halves arrive as one packet.
        client.enqueueSpeakerAudio(QByteArray(reinterpret_cast<const char*>(half.data()),
                                              static_cast<qsizetype>(half.size() * sizeof(std::int16_t))));
        P2ClientTestAccess::drain(client);
        check(collect(listener, 1).size() == 1, "the completing half releases one packet");
        client.stop();
    }

    // ---- the pacer bounds the burst, and the sequence keeps counting ----
    {
        P2Client client;
        client.start(loopbackParams(true));
        client.enqueueSpeakerAudio(blockOf(0x0101, 20));
        P2ClientTestAccess::drain(client);
        auto got = collect(listener, 16);
        check(got.size() == 16,
              "twenty queued packets release only the sixteen-packet target");
        bool ordered = true;
        for (std::size_t i = 0; i < got.size(); ++i) {
            if (sequenceOf(got[i]) != i) ordered = false;
        }
        check(ordered, "sequence numbers are consecutive from zero, in order");

        // At target with NO drain time, nothing more goes.
        P2ClientTestAccess::drain(client);
        check(collect(listener, 0).empty(), "a full FIFO estimate releases nothing");

        // Exactly two packets' worth of drain time releases exactly two packets.
        P2ClientTestAccess::drain(client, P2ClientTestAccess::packetSeconds(2));
        got = collect(listener, 2);
        check(got.size() == 2, "two packets' worth of drain time releases two packets");
        check(!got.empty() && sequenceOf(got[0]) == 16,
              "the sequence continues across drains rather than restarting");
        client.stop();
    }

    // ---- overflow drops the OLDEST, so the newest audio survives ----
    {
        P2Client client;
        client.start(loopbackParams(true));
        // Fill well past the queue cap without draining. Each block is tagged, so
        // what comes out identifies which audio was kept.
        for (int i = 1; i <= 80; ++i) {
            client.enqueueSpeakerAudio(blockOf(static_cast<std::int16_t>(i)));
        }
        P2ClientTestAccess::drain(client);
        const auto got = collect(listener, 16);
        check(!got.empty(), "an overflowed queue still sends");
        // The last block enqueued was tagged 80. Dropping from the front means
        // what remains is the TAIL, so the first packet out must be far from 1.
        // If the policy were "drop the newest", the first packet out would be 1.
        check(!got.empty() && firstSampleOf(got[0]) > 1,
              "the oldest audio was dropped, not the newest");
        client.stop();
    }

    // ---- the enable gate, and what stop() clears ----
    {
        P2Client client;
        client.start(loopbackParams(false));   // speaker audio OFF
        client.enqueueSpeakerAudio(blockOf(0x0F0F));
        check(P2ClientTestAccess::pendingSamples(client) == 0,
              "with the stream disabled, audio is not even queued");
        P2ClientTestAccess::drain(client);
        check(collect(listener, 0).empty(), "a disabled stream sends nothing");
        client.stop();
    }
    {
        P2Client client;
        client.start(loopbackParams(true));
        // MORE PACKETS THAN THE TARGET RELEASES, so some are still queued when
        // stop() runs. Twice now this has had to be corrected: it first queued
        // three against a four-packet target, and the S3 mutation caught that all
        // three were sent and stop() had nothing to clear. Raising the target to
        // sixteen broke it again the same way. Derived from the constant rather
        // than written as a literal, so the next change to the target cannot
        // silently hollow this check out a third time.
        const int overTarget =
            static_cast<int>(SpeakerAudioPacer::kTargetFifoFrames / kSpeakerFramesPerPacket) + 4;
        client.enqueueSpeakerAudio(blockOf(0x2222, overTarget));
        P2ClientTestAccess::drain(client);
        (void)collect(listener, overTarget - 4);
        check(P2ClientTestAccess::pendingSamples(client) > 0,
              "audio is still queued, so stop() has something to drop");
        check(P2ClientTestAccess::sequence(client) > 0, "sequence advanced during the session");
        client.stop();
        check(P2ClientTestAccess::pendingSamples(client) == 0,
              "stop() drops queued audio rather than playing it into the next session");
        check(P2ClientTestAccess::sequence(client) == 0,
              "stop() restarts the sequence, since it is per-session");

        // Enqueueing after stop must not resurrect the stream.
        client.enqueueSpeakerAudio(blockOf(0x3333));
        check(P2ClientTestAccess::pendingSamples(client) == 0,
              "a stopped client queues nothing");
    }

    // ---- the radio's underflow report is the verdict on our pacing ----
    {
        P2Client client;
        client.start(loopbackParams(true));
        check(client.speakerUnderflowReports() == 0,
              "a fresh session has reported no underflow");

        std::vector<std::uint8_t> ok(kHighPriorityStatusBytes, 0);
        ok[37] = 0x02; ok[38] = 0x00;              // level 512, no underflow bit
        P2ClientTestAccess::feedDatagram(client, ok, 1025);
        check(client.speakerUnderflowReports() == 0,
              "a healthy status packet reports no underflow");
        check(client.lastSpeakerFifoLevel() == 512, "the level is recorded for the bench");

        std::vector<std::uint8_t> dry(kHighPriorityStatusBytes, 0);
        dry[30] = 0b0000'1000;
        dry[3] = 1;                                 // sequence 1, so not a discovery reply
        P2ClientTestAccess::feedDatagram(client, dry, 1025);
        check(client.speakerUnderflowReports() == 1, "an underflow report is counted");
        P2ClientTestAccess::feedDatagram(client, dry, 1025);
        check(client.speakerUnderflowReports() == 2,
              "repeats are counted rather than collapsed, so a starved stream is visible");

        client.stop();
        check(client.speakerUnderflowReports() == 0,
              "stop() clears the count, since it describes one session");
    }

    // ---- a Discovery reply is never a fault report, however often it arrives ----
    //
    // Both packets are 60 bytes, and a reply's byte 30 bit 3 means nothing at all.
    // If the reply parse were gated on "discovery already announced", the SECOND
    // reply -- a duplicate, a retransmit, a reply to someone else's broadcast on
    // this socket -- would fall through to the status parse and be counted as the
    // radio's speaker FIFO running dry. That invents a fault from a packet that
    // carries no fault, and it is the one report this stream's pacing is judged on.
    {
        P2Client client;
        client.start(loopbackParams(true));

        // A real reply with bit 3 of byte 30 set: as a reply those bits are not a
        // field at all, so nothing here may be read as an underflow.
        std::vector<std::uint8_t> reply(kHighPriorityStatusBytes, 0);
        reply[4] = 0x03;             // Discovery reply, radio streaming
        reply[11] = 10;              // board id
        reply[13] = 27;              // firmware
        reply[20] = 1;               // one DDC
        reply[30] = 0b0000'1000;     // would be the speaker-underflow bit in a status
        reply[37] = 0x03; reply[38] = 0xE8;   // would be level 1000 in a status

        P2ClientTestAccess::feedDatagram(client, reply, 1025);
        check(client.speakerUnderflowReports() == 0 && client.lastSpeakerFifoLevel() == 0,
              "the first Discovery reply is not decoded as speaker status");

        // The same datagram again, now that discovery has been announced. This is
        // the case the guard used to let through.
        P2ClientTestAccess::feedDatagram(client, reply, 1025);
        P2ClientTestAccess::feedDatagram(client, reply, 1025);
        check(client.speakerUnderflowReports() == 0,
              "and a repeated reply still reports no underflow, the parse not being gated");
        check(client.lastSpeakerFifoLevel() == 0,
              "nor does it overwrite the FIFO level with bytes that are not a level");

        // A genuine status packet still lands, so the fix did not swallow the real
        // thing: byte 4 is a PTT bitfield of 0, which no reply can have.
        std::vector<std::uint8_t> dry(kHighPriorityStatusBytes, 0);
        dry[3] = 9;
        dry[30] = 0b0000'1000;
        P2ClientTestAccess::feedDatagram(client, dry, 1025);
        check(client.speakerUnderflowReports() == 1,
              "a real status packet is still decoded after any number of replies");
        client.stop();
    }

    // ---- with the stream off, the radio's dry FIFO is not our fault ----
    {
        P2Client client;
        client.start(loopbackParams(false));
        std::vector<std::uint8_t> dry(kHighPriorityStatusBytes, 0);
        dry[30] = 0b0000'1000;
        dry[3] = 1;
        P2ClientTestAccess::feedDatagram(client, dry, 1025);
        check(client.speakerUnderflowReports() == 1,
              "the report is still counted, because it did arrive");
        check(client.lastSpeakerFifoLevel() == 0, "and the level is still recorded");
        client.stop();
    }

    if (g_failures == 0)
        std::fprintf(stderr, "anan_speaker_audio_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
