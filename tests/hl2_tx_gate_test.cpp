// MetisClient used to be incapable of keying a radio, structurally: every C0
// register-address byte was even, so MOX (C0 bit 0) was always 0. Adding TX
// destroyed that invariant. This test is what replaces it.
//
// The claim under test is not "setMox returns false when refused" -- it is the
// only one that actually matters on the air: WITH THE GATE CLOSED, NO BYTE THAT
// WOULD GO OUT ON THE WIRE EVER HAS C0 BIT 0 SET. So it inspects the real EP2
// packet the client would send, from the same builder the socket path uses,
// rather than trusting a status flag.

#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "TxTestAuthority.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>

#include <cstdio>
#include <vector>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    // No start(), bind(), peer or datagrams: inject streaming state and inspect
    // packets using the same builder as the transport.
    static void setStreaming(MetisClient& client) { client.m_running = true; }
    static void writer(MetisClient& client,
                       std::function<qint64(const std::array<std::uint8_t, kUsbPacketSize>&)> sink)
    {
        client.m_packetSinkForTest = std::move(sink);
    }
    static void send(MetisClient& client) { client.sendControlPacket(); }
};

struct Hl2TxGateTestAccess {
    static void prepare(Hl2Backend& backend)
    {
        // Constructed transport only: no connectRadio(), socket, peer or DSP
        // setup. Exercise the real backend's queued CW/hang paths.
        backend.m_txAllowed = true;
        QMetaObject::invokeMethod(backend.m_metis, [metis = backend.m_metis] {
            metis->enableTransmit(true);
        }, Qt::BlockingQueuedConnection);
        backend.setSliceMode(0, QStringLiteral("CW"));
    }

    static std::array<std::uint8_t, kUsbPacketSize> packet(Hl2Backend& backend)
    {
        std::array<std::uint8_t, kUsbPacketSize> packet{};
        QMetaObject::invokeMethod(backend.m_metis, [&packet, metis = backend.m_metis] {
            packet = metis->buildNextControlPacket();
        }, Qt::BlockingQueuedConnection);
        return packet;
    }

    static void expireHang(Hl2Backend& backend)
    {
        backend.m_cwHangTimer->stop();
        QMetaObject::invokeMethod(backend.m_cwHangTimer, "timeout", Qt::DirectConnection);
    }
};
}

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

// Both sub-frames carry C0; keying either one keys the radio, so both are
// checked. Frame C0 sits at SYNC(3) into each 512-byte frame.
static bool anyFrameKeyed(const std::array<std::uint8_t, kUsbPacketSize>& pkt)
{
    const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
    for (const std::size_t fs : frameStarts) {
        if ((pkt[fs + 3] & kC0MoxBit) != 0)
            return true;
    }
    return false;
}

static bool payloadNonZero(const std::array<std::uint8_t, kUsbPacketSize>& pkt)
{
    const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
    for (const std::size_t fs : frameStarts) {
        const std::uint8_t* pay = pkt.data() + fs + 8;
        for (std::size_t k = 0; k < kFramePayload; ++k) {
            if (pay[k] != 0) {
                return true;
            }
        }
    }
    return false;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    TxTestAuthority authority;
    MetisClient client;

    {
        MetisClient board;
        const auto isBoardWrite = [](const auto& packet) {
            return (packet[8 + kFrameSize + 3] & ~kC0MoxBit) == kC0I2c2;
        };
        board.setIoBoardTxFrequencyHz(7'100'000);
        check(!isBoardWrite(board.buildNextControlPacket()),
              "disconnected IO-board request queues nothing");
        MetisClientTestAccess::setStreaming(board);
        board.setIoBoardTxFrequencyHz(7'100'000);
        check(isBoardWrite(board.buildNextControlPacket()), "connected IO-board push reaches packet builder");
        board.stop(); // interrupt after only the MSB: four stale banks remain
        for (int i = 0; i < 8; ++i) {
            check(!isBoardWrite(board.buildNextControlPacket()),
                  "stop discards every unfinished IO-board bank");
        }
        MetisClientTestAccess::setStreaming(board);
        board.setIoBoardTxFrequencyHz(7'100'000);
        for (int reg = 0; reg < 5; ++reg) {
            const auto packet = board.buildNextControlPacket();
            check(isBoardWrite(packet), "same frequency is resent after session reset");
            check(packet[8 + kFrameSize + 6] == reg, "session restarts at MSB and commits LSB last");
            check(!anyFrameKeyed(packet), "IO-board writes never key an unkeyed transmitter");
        }
        board.enableTransmit(true);
        board.setMox(true, authority.operation);
        board.setIoBoardTxFrequencyHz(14'225'000);
        bool sawBoard = false;
        for (int i = 0; i < 12; ++i) {
            const auto packet = board.buildNextControlPacket();
            sawBoard = sawBoard || isBoardWrite(packet);
            check(anyFrameKeyed(packet), "IO-board update preserves explicit key state");
        }
        check(sawBoard, "IO-board band update is not withheld while keyed");
        board.setMox(false, authority.operation);
    }

    check(!client.transmitEnabled(), "transmit is DISABLED by default");
    check(!client.isKeyed(), "not keyed by default");

    // ---- gate closed ----
    client.setMox(true, authority.operation);
    check(!client.isKeyed(), "setMox(true) refused while the gate is closed");

    // Drain enough packets to cover the whole round robin (freq, gain, ADC
    // assign) plus the config bank, several times over, with a key request
    // standing the entire time. Any single keyed frame here is a real radio
    // keyed by accident.
    for (int i = 0; i < 64; ++i) {
        client.setMox(true, authority.operation);                       // keep asking, every frame
        const auto pkt = client.buildNextControlPacket();
        if (anyFrameKeyed(pkt)) {
            check(false, "a frame was keyed with the gate CLOSED");
            break;
        }
    }

    // Queuing TX frequency and drive must not key anything either -- those are
    // setup, and setup happening before the operator keys is the normal order.
    // #4449: a non-zero drive requested while the gate is CLOSED must also NOT
    // assert the PA-enable bit (C2 DATA[19] = 0x08) on the wire -- MetisClient
    // forces drive 0 / PA off, so connecting can never bias the PA on in a
    // transmit-blocked session.
    client.setTxFrequencyHz(14'200'000);
    client.setTxDriveLevel(200);
    bool sawDriveBank = false, paEnabledWhileClosed = false;
    for (int i = 0; i < 8; ++i) {   // same packet count as before, to keep the round-robin phase stable
        const auto pkt = client.buildNextControlPacket();
        if (anyFrameKeyed(pkt)) {
            check(false, "TX frequency/drive setup keyed a frame with the gate closed");
            break;
        }
        const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
        for (const std::size_t fs : frameStarts) {
            if ((pkt[fs + 3] & ~kC0MoxBit) == kC0TxDrive) {
                sawDriveBank = true;
                if ((pkt[fs + 5] & 0x08) != 0) paEnabledWhileClosed = true;   // C2 PA-enable
            }
        }
    }
    check(sawDriveBank, "the drive C&C bank reaches the wire after setTxDriveLevel");
    check(!paEnabledWhileClosed,
          "#4449: PA-enable stays clear when drive is set with the gate CLOSED");

    // ---- gate open ----
    client.enableTransmit(true);
    check(client.transmitEnabled(), "transmit enabled after explicit opt-in");
    check(!client.isKeyed(), "opening the gate does not key by itself");

    // Still unkeyed until asked.
    for (int i = 0; i < 4; ++i) {
        if (anyFrameKeyed(client.buildNextControlPacket())) {
            check(false, "an open gate keyed a frame without setMox");
            break;
        }
    }

    client.setMox(true, authority.operation);
    check(client.isKeyed(), "setMox(true) honoured once the gate is open");
    {
        const auto pkt = client.buildNextControlPacket();
        const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
        // BOTH sub-frames must carry MOX: the radio keys off whichever bank is
        // in flight, so keying only one is an intermittent, cadence-dependent
        // half-key -- the worst possible failure mode to debug.
        for (const std::size_t fs : frameStarts) {
            check((pkt[fs + 3] & kC0MoxBit) != 0, "both sub-frames carry MOX when keyed");
            // The address must survive keying, or MOX would corrupt the bank.
            check((pkt[fs + 3] & ~kC0MoxBit) != 0 || fs == 8,
                  "register address intact alongside MOX");
        }
    }

    // ---- unkey, and revoking the gate ----
    client.setMox(false, authority.operation);
    check(!anyFrameKeyed(client.buildNextControlPacket()), "unkeys cleanly");

    // Revoking the gate while keyed must drop the key on the wire immediately,
    // not merely refuse the next request.
    client.setMox(true, authority.operation);
    check(anyFrameKeyed(client.buildNextControlPacket()), "keyed again");
    client.enableTransmit(false);
    for (int i = 0; i < 8; ++i) {
        if (anyFrameKeyed(client.buildNextControlPacket())) {
            check(false, "revoking the gate did not drop the key on the wire");
            break;
        }
    }

    // ---- transmit IQ reaches the wire only while keyed ----
    //
    // The gate has to cover SAMPLES, not just MOX. A radio that is unkeyed but
    // still being fed IQ is not transmitting, but it is one stray MOX bit away
    // from doing so with whatever happens to be in the buffer.
    {
        MetisClient c2;
        std::vector<std::complex<float>> tone(512, std::complex<float>(0.5f, -0.5f));

        // Gate closed, unkeyed: queued samples must not go out.
        c2.queueTxIq(tone, authority.context);
        check(c2.txQueueDepth() == 512, "samples queue regardless of gate state");
        for (int i = 0; i < 4; ++i) {
            if (payloadNonZero(c2.buildNextControlPacket())) {
                check(false, "IQ reached the wire with the gate CLOSED");
                break;
            }
        }
        check(c2.txQueueDepth() == 512, "an unkeyed frame consumes no samples");

        // Gate open but still unkeyed: still silence.
        c2.enableTransmit(true);
        check(!payloadNonZero(c2.buildNextControlPacket()),
              "an open gate alone does not put IQ on the wire");

        // Keyed: now the samples flow.
        c2.setMox(true, authority.operation);
        const auto keyedPkt = c2.buildNextControlPacket();
        check(payloadNonZero(keyedPkt), "keyed frames carry the queued IQ");
        check(c2.txQueueDepth() == 512 - kTxSamplesPerPacket,
              "one packet consumes exactly kTxSamplesPerPacket samples");
        // 0.5 -> 16383 (0x3FFF), -0.5 -> -16383 (0xC001), big-endian.
        const std::uint8_t* pay = keyedPkt.data() + 8 + 8;
        check(pay[4] == 0x3F && pay[5] == 0xFF, "I sample encoded big-endian");
        check(pay[6] == 0xC0 && pay[7] == 0x01, "Q sample encoded big-endian");
        check(pay[0] == 0 && pay[1] == 0 && pay[2] == 0 && pay[3] == 0,
              "EADDR still zero with IQ present");

        // Unkey must discard pending audio, not carry it into the next
        // transmission. Measured on hardware before this existed: a key with no
        // audio still produced ~1000 counts of forward power for a moment.
        c2.queueTxIq(tone, authority.context);
        check(c2.txQueueDepth() > 0, "audio queued");
        c2.flushTxIq();
        check(c2.txQueueDepth() == 0, "flushTxIq discards pending transmit audio");
        check(!payloadNonZero(c2.buildNextControlPacket()),
              "nothing left to transmit after a flush");
        c2.queueTxIq(tone, authority.context);

        // Underflow is silence, not a stall and not repeated stale audio.
        while (c2.txQueueDepth() > 0)
            c2.buildNextControlPacket();
        check(!payloadNonZero(c2.buildNextControlPacket()),
              "an empty queue transmits silence rather than repeating");
    }

    // ---- PC/MIDI CW is a shaped, packet-paced carrier under MOX ----
    {
        MetisClient cw;
        cw.setCwKeyDown(true, authority.operation);
        check(!cw.cwModeActive(),
              "CW down is not latched while the transmit gate is closed");
        check(!payloadNonZero(cw.buildNextControlPacket()),
              "a refused CW edge puts no samples on the wire");

        cw.enableTransmit(true);
        cw.setCwKeyDown(true, authority.operation);
        check(cw.cwModeActive() && cw.cwKeyDown(),
              "an allowed CW down edge enters software-CW mode");
        check(!payloadNonZero(cw.buildNextControlPacket()),
              "CW still requires MOX — break-in policy belongs to the backend");

        cw.setMox(true, authority.operation);
        bool sawCarrier = false;
        for (int i = 0; i < 5; ++i) {
            sawCarrier |= payloadNonZero(cw.buildNextControlPacket());
        }
        check(sawCarrier, "key-down emits the raised-cosine CW carrier under MOX");

        // Voice queued behind manual PTT must not leak between CW elements.
        std::vector<std::complex<float>> voice(512, std::complex<float>(0.4f, -0.4f));
        cw.queueTxIq(voice, authority.context);
        cw.setCwKeyDown(false, authority.operation);
        for (int i = 0; i < 6; ++i) {
            cw.buildNextControlPacket();
        }
        check(!payloadNonZero(cw.buildNextControlPacket()),
              "key-up finishes its five-ms fall then emits silence, not queued voice");

        cw.clearCwKeying();
        check(!cw.cwModeActive() && !cw.cwKeyDown(),
              "ending the CW PTT envelope releases IQ ownership");
        check(!payloadNonZero(cw.buildNextControlPacket()),
              "ending CW drops microphone IQ queued during the element sequence");
        cw.queueTxIq(voice, authority.context);
        check(payloadNonZero(cw.buildNextControlPacket()),
              "normal transmit IQ resumes after CW mode is cleared");
    }

    {
        TxTestAuthority media;
        MetisClient queued;
        queued.enableTransmit(true);
        queued.setMox(true, media.operation);
        const std::vector<std::complex<float>> voice(252, {0.25f, 0.25f});
        queued.queueTxIq(voice, media.context);
        (void)media.coordinator.finishLocalIntent(media.operation);
        const auto operation = media.coordinator.acquire(media.actor, AetherSDR::TxCoordinator::monotonicMs()).operation;
        const auto context = media.coordinator.mediaContext(media.producer, operation);
        check(!payloadNonZero(queued.buildNextControlPacket()) && queued.txQueueDepth() == 0,
              "a new operation cannot consume old queued HL2 IQ");
        queued.setMox(true, operation);
        queued.queueTxIq(voice, context);
        queued.queueTxIq(voice, media.context);
        check(queued.txQueueDepth() == voice.size(), "late old IQ cannot append to the new producer queue");
        media.producer.invalidate();
        check(!payloadNonZero(queued.buildNextControlPacket()), "producer teardown fences already queued HL2 IQ");
    }

    {
        TxTestAuthority tx;
        MetisClient fenced;
        fenced.enableTransmit(true);
        fenced.setMox(true, {});
        check(!anyFrameKeyed(fenced.buildNextControlPacket()),
              "opening the transport gate never substitutes for an admitted operation");
        fenced.setMox(true, tx.operation);
        fenced.setTxTestTone(0.0, 0.5, tx.operation);
        check(payloadNonZero(fenced.buildNextControlPacket()), "admitted tune carrier reaches the packet builder");
        (void)tx.coordinator.finishLocalIntent(tx.operation);
        const auto fresh = tx.coordinator.acquire(tx.actor, AetherSDR::TxCoordinator::monotonicMs()).operation;
        fenced.setMox(true, fresh);
        fenced.setMox(false, tx.operation);
        fenced.setCwKeyDown(true, tx.operation);
        fenced.setTxTestTone(0.0, 0.9, tx.operation);
        const auto voice = fenced.buildNextControlPacket();
        check(anyFrameKeyed(voice) && !payloadNonZero(voice),
              "old key-up, CW and tone queues neither unkey nor modulate a fresh voice operation");
        fenced.setCwKeyDown(true, fresh);
        check(payloadNonZero(fenced.buildNextControlPacket()), "fresh CW still produces shaped IQ");
        (void)tx.coordinator.cancel(tx.actor, fresh);
        const auto stopped = fenced.buildNextControlPacket();
        check(!anyFrameKeyed(stopped) && !payloadNonZero(stopped),
              "cancellation fences latched MOX and internally generated CW at the packet builder");
    }

    {
        TxTestAuthority tx;
        MetisClient held;
        held.enableTransmit(true);
        const auto first = tx.coordinator.registerProducer();
        const auto second = tx.coordinator.registerProducer();
        const auto a = first.request();
        const auto b = second.request();
        const auto aIntent = tx.coordinator.beginRequest(a, tx.operation,
            AetherSDR::TxCoordinator::Activity::Mox);
        const auto bIntent = tx.coordinator.beginRequest(b, tx.operation,
            AetherSDR::TxCoordinator::Activity::Mox);
        held.setMox(true, tx.coordinator.requestOperation(a));
        held.setMox(true, tx.coordinator.requestOperation(b));
        (void)tx.coordinator.endIntent(bIntent);
        second.invalidate();
        check(anyFrameKeyed(held.buildNextControlPacket()),
              "releasing the most recent contributor preserves another live MOX hold");
        first.invalidate();
        check(!anyFrameKeyed(held.buildNextControlPacket()),
              "last producer death immediately fences held MOX before queued cleanup");
        held.setMox(true, tx.coordinator.requestOperation(b));
        check(!anyFrameKeyed(held.buildNextControlPacket()),
              "sustaining an existing latch cannot admit a stale queued key-on");
        (void)tx.coordinator.endIntent(aIntent);
    }

    {
        TxTestAuthority tx;
        Hl2Backend cw;
        Hl2TxGateTestAccess::prepare(cw);
        const auto first = tx.coordinator.registerProducer();
        const auto second = tx.coordinator.registerProducer();
        const auto a = first.request();
        const auto b = second.request();
        const auto aIntent = tx.coordinator.beginRequest(a, tx.operation,
            AetherSDR::TxCoordinator::Activity::CwKey);
        const auto bIntent = tx.coordinator.beginRequest(b, tx.operation,
            AetherSDR::TxCoordinator::Activity::CwKey);
        cw.setCwKeying(true, true, 500, tx.coordinator.requestOperation(a));
        cw.setCwKeying(true, true, 500, tx.coordinator.requestOperation(b));
        check(payloadNonZero(Hl2TxGateTestAccess::packet(cw)), "overlapping CW holds produce a carrier");
        // RadioModel suppresses the global up while a compatible hold remains.
        (void)tx.coordinator.closeRequest(b);
        (void)tx.coordinator.endIntent(bIntent);
        second.invalidate();
        const auto remaining = Hl2TxGateTestAccess::packet(cw);
        check(anyFrameKeyed(remaining) && payloadNonZero(remaining),
              "ending the latest CW contributor preserves another held element");
        first.invalidate();
        const auto stopped = Hl2TxGateTestAccess::packet(cw);
        check(!anyFrameKeyed(stopped) && !payloadNonZero(stopped),
              "last CW producer invalidation fences carrier and break-in MOX before cleanup");
        (void)tx.coordinator.endIntent(aIntent);
    }

    {
        TxTestAuthority tx;
        Hl2Backend cw;
        Hl2TxGateTestAccess::prepare(cw);
        const auto producer = tx.coordinator.registerProducer();
        const auto a = producer.request();
        const auto aIntent = tx.coordinator.beginRequest(a, tx.operation,
            AetherSDR::TxCoordinator::Activity::CwKey);
        const auto aOperation = tx.coordinator.requestOperation(a);
        cw.setCwKeying(true, true, 500, aOperation);
        check(payloadNonZero(Hl2TxGateTestAccess::packet(cw)), "first scoped CW element reaches the packet builder");
        (void)tx.coordinator.closeRequest(a);
        int completed = 0;
        cw.setCwKeying(false, true, 500, aOperation,
            AetherSDR::TxCoordinator::Completion([&] {
                QMetaObject::invokeMethod(&app, [&] {
                    (void)tx.coordinator.endIntent(aIntent);
                    ++completed;
                }, Qt::AutoConnection);
            }));
        (void)Hl2TxGateTestAccess::packet(cw); // consume the queued up; hang retains completion
        check(completed == 0, "CW release completion waits for its normal break-in hang");
        const auto b = producer.request();
        const auto bIntent = tx.coordinator.beginRequest(b, tx.operation,
            AetherSDR::TxCoordinator::Activity::CwKey);
        const auto bOperation = tx.coordinator.requestOperation(b);
        cw.setCwKeying(true, true, 500, bOperation);
        const auto next = Hl2TxGateTestAccess::packet(cw);
        check(completed == 1 && anyFrameKeyed(next) && payloadNonZero(next),
              "a successive scoped CW element survives completion of the preceding hang");
        (void)tx.coordinator.closeRequest(b);
        cw.setCwKeying(false, true, 500, bOperation,
            AetherSDR::TxCoordinator::Completion([&] {
                QMetaObject::invokeMethod(&app, [&] {
                    (void)tx.coordinator.endIntent(bIntent);
                    ++completed;
                }, Qt::AutoConnection);
            }));
        (void)Hl2TxGateTestAccess::packet(cw);
        Hl2TxGateTestAccess::expireHang(cw);
        const auto stopped = Hl2TxGateTestAccess::packet(cw);
        QCoreApplication::sendPostedEvents();
        check(completed == 2 && !anyFrameKeyed(stopped) && !payloadNonZero(stopped),
              "the final scoped CW hang still unkeys and clears its carrier");
    }

    {
        TxTestAuthority tx;
        MetisClient independent;
        independent.enableTransmit(true);
        const auto manual = tx.coordinator.registerProducer();
        const auto keyer = tx.coordinator.registerProducer();
        const auto ptt = manual.request();
        const auto element = keyer.request();
        const auto pttIntent = tx.coordinator.beginRequest(ptt, tx.operation,
            AetherSDR::TxCoordinator::Activity::Mox);
        const auto cwIntent = tx.coordinator.beginRequest(element, tx.operation,
            AetherSDR::TxCoordinator::Activity::CwKey);
        independent.setMox(true, tx.coordinator.requestOperation(ptt));
        independent.setCwKeyDown(true, tx.coordinator.requestOperation(element));
        check(payloadNonZero(independent.buildNextControlPacket()), "CW can ride separately admitted manual MOX");
        (void)tx.coordinator.endIntent(pttIntent);
        const auto released = independent.buildNextControlPacket();
        check(!anyFrameKeyed(released) && !payloadNonZero(released),
              "a bare CW element never sustains a released manual MOX hold");
        (void)tx.coordinator.endIntent(cwIntent);
        (void)independent.buildNextControlPacket();
        independent.setCwKeyDown(true, tx.coordinator.requestOperation(element));
        check(!independent.cwKeyDown(), "an ended CW request cannot relatch its carrier");
    }

    {
        TxTestAuthority tx;
        MetisClient entered;
        entered.enableTransmit(true);
        entered.setMox(true, tx.operation);
        bool written = false;
        MetisClientTestAccess::writer(entered, [&](const auto& packet) -> qint64 {
            written = anyFrameKeyed(packet);
            check(tx.coordinator.hasInFlightDispatches(), "Metis terminal control writer holds its dispatch guard");
            (void)tx.coordinator.cancel(tx.actor, tx.operation);
            check(!tx.coordinator.acknowledgeStopped(tx.operation),
                  "reentrant teardown cannot acknowledge an entered Metis write");
            return packet.size();
        });
        MetisClientTestAccess::send(entered);
        check(written && tx.coordinator.acknowledgeStopped(tx.operation),
              "Metis writer guard ends only after the terminal writer returns");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_tx_gate_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
