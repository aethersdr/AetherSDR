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
#include "core/backends/hl2/Hl2HardwareOptions.h"
#include "core/backends/hl2/Hl2TxDsp.h"
#include "TxTestAuthority.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <cstdio>
#include <limits>
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
    // kTxQueueMax is private and deliberately stays that way; the overflow
    // check needs the real bound rather than a retyped copy of it, because a
    // test that retypes a constant agrees with itself while the code it guards
    // moves underneath.
    static constexpr std::size_t queueMax() { return MetisClient::kTxQueueMax; }
    // Same reason as queueMax(). kEp2AudioRateHz is a private constant of
    // MetisClient and NOT one of MetisProtocol.h's -- the key-down burst below
    // is computed from the real EP2 rate, not from a 48000 typed into a test.
    static constexpr int ep2AudioRateHz() { return MetisClient::kEp2AudioRateHz; }
    // The gap that ends a starvation EPISODE, read from the production
    // constant so the drift-shape assertion cannot drift away from it.
    static constexpr std::uint64_t runQuietPackets()
    {
        return MetisClient::kUnderflowRunQuietPackets;
    }
};

struct Hl2TxGateTestAccess {
    // Declare the gateware ATU without going near the settings store, and read
    // the transmit gate back so a test can state which side of it it is on.
    static void declareGatewareAtu(Hl2Backend& backend)
    {
        Hl2HardwareOptions hw = backend.m_hw;
        hw.atuGateware = true;
        backend.m_hw = hw;
    }
    static bool txAllowed(const Hl2Backend& backend) { return backend.m_txAllowed; }
    // Set it explicitly rather than relying on the default. m_txAllowed is
    // `!automation || automationAllowsTx`, so in a plain test process it is
    // TRUE — the closed gate is the automation-bridge-without-ALLOW_TX case,
    // and a test that wants it has to say so.
    static void setTxAllowed(Hl2Backend& backend, bool allowed) { backend.m_txAllowed = allowed; }

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

// ---- THE SEAM THAT GIVES reportTxUnderflowRun() AN ASSERTION ----
//
// Without this the function is the one piece of new logic nothing observes:
// its run members are private, nothing reads the log, and emptying its body
// plus deleting its call sites leaves every counter assertion in this file
// green. That is a test agreeing with itself by omission. The repo already has
// the shape -- tests/tci_server_review_test.cpp installs a qInstallMessageHandler
// for exactly this reason -- so it is used here rather than invented.
//
// The category has to be turned ON explicitly: "aether.hl2.tx" is high-rate and
// the product disables it by default, which is the whole point of its separate
// toggle in LogManager.
static QStringList* g_logSink = nullptr;
static void captureLogHandler(QtMsgType, const QMessageLogContext&, const QString& msg)
{
    if (g_logSink)
        *g_logSink << msg;
}

class ScopedTxFifoLog
{
public:
    ScopedTxFifoLog()
    {
        g_logSink = &m_lines;
        m_previous = qInstallMessageHandler(captureLogHandler);
        QLoggingCategory::setFilterRules(QStringLiteral("aether.hl2.tx.debug=true"));
    }
    ~ScopedTxFifoLog()
    {
        QLoggingCategory::setFilterRules(QString());
        qInstallMessageHandler(m_previous);
        g_logSink = nullptr;
    }
    // Only the starvation lines: "aether.hl2.tx" is shared with Hl2Backend's
    // gateware telemetry and Hl2TxDsp's modulator, on purpose, so the filter
    // is on the message and not on the category.
    [[nodiscard]] QStringList starvationLines() const
    {
        QStringList out;
        for (const QString& line : m_lines) {
            if (line.contains(QStringLiteral("HOST queue starved")))
                out << line;
        }
        return out;
    }

private:
    QStringList m_lines;
    QtMessageHandler m_previous = nullptr;
};

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

// THE WIRE SIGNATURE OF A STARVATION, as opposed to the counter's report of
// one. A short block leaves a run of zeroed samples at the TAIL of the EP2
// packet -- which is exactly the shape FIND-23 measured on the captures (48,
// 18 and 64 trailing zeros, "all pure-trailing, none leading or interior").
// Asserting this is what keeps the underflow checks from merely recomputing
// the same arithmetic the production code just performed.
//
// LIMIT, because the helper cannot tell them apart: a genuine audio sample
// that happens to be exactly zero reads as silence here. Every caller below
// feeds a constant non-zero value for that reason.
struct Ep2IqShape {
    int sampleSlots = 0;     // sample slots in the packet (kTxSamplesPerPacket)
    int carryingIq = 0;      // slots with a non-zero I or Q
    int firstSilent = -1;    // index of the first all-zero slot, -1 if none
};

static Ep2IqShape ep2IqShape(const std::array<std::uint8_t, kUsbPacketSize>& pkt)
{
    Ep2IqShape shape;
    const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
    for (const std::size_t fs : frameStarts) {
        const std::uint8_t* pay = pkt.data() + fs + 8;
        for (std::size_t k = 0; k + kTxSampleBytes <= kFramePayload;
             k += kTxSampleBytes, ++shape.sampleSlots) {
            const bool nonZero = pay[k + 4] || pay[k + 5] || pay[k + 6] || pay[k + 7];
            if (nonZero) {
                ++shape.carryingIq;
            } else if (shape.firstSilent < 0) {
                shape.firstSilent = shape.sampleSlots;
            }
        }
    }
    return shape;
}

// THE ATU TUNE REQUEST IS TRANSMIT, AND THE GATE MUST REACH IT. Principle VI.
//
// setTune(true) raises 0x09[20] BEFORE the carrier, deliberately. Every step
// after it — applyDrive(), setTxTestTone(), setKeying() — refuses when the
// transmit gate is closed. This one did not, and it is not inert: the gateware's
// tuner state machine leaves IDLE on the BIT ALONE (exttuner.v,
// `IDLE: if (enable) state_next = DELAY;` — no key in that transition), then
// asserts `txinhibit` into `tx_en`/`cw_on` and drives `start` low, which is the
// request that tells an external tuner to begin a tune cycle.
//
// So with the bridge active and no AETHER_AUTOMATION_ALLOW_TX, pressing TUNE
// started a real antenna tuner into whatever was connected. Raised three times
// by @on8st as a symmetry point on #5867; the gateware says it was a defect.
static void testAtuTuneHonoursTheTransmitGate(TxTestAuthority& authority)
{
    // C2 of the 0x09 bank; the tune request is bit 4 (DATA[20]).
    const auto tuneRequestedIn = [](const std::array<std::uint8_t, kUsbPacketSize>& pkt) {
        const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
        for (const std::size_t fs : frameStarts) {
            if ((pkt[fs + 3] & ~kC0MoxBit) == kC0TxDrive && (pkt[fs + 5] & 0x10) != 0)
                return true;
        }
        return false;
    };

    const auto sawTuneRequest = [&](Hl2Backend& backend) {
        // The push to MetisClient is a queued invoke onto its own thread, so let
        // it land before reading the builder.
        QCoreApplication::processEvents();
        for (int i = 0; i < 12; ++i) {
            if (tuneRequestedIn(Hl2TxGateTestAccess::packet(backend)))
                return true;
        }
        return false;
    };

    {
        Hl2Backend backend;
        Hl2TxGateTestAccess::prepare(backend);                  // transport, DSP
        Hl2TxGateTestAccess::declareGatewareAtu(backend);
        Hl2TxGateTestAccess::setTxAllowed(backend, false);      // the bridge case
        check(!Hl2TxGateTestAccess::txAllowed(backend), "the transmit gate is closed");
        backend.setTune(true, 10, authority.operation, {});
        check(!sawTuneRequest(backend),
              "gate CLOSED: TUNE puts no ATU tune request on the wire — an external "
              "tuner is not started by a session that may not transmit");
        backend.setTune(false, 10, authority.operation, {});
    }

    {
        Hl2Backend backend;
        Hl2TxGateTestAccess::prepare(backend);
        Hl2TxGateTestAccess::declareGatewareAtu(backend);
        Hl2TxGateTestAccess::setTxAllowed(backend, true);
        check(Hl2TxGateTestAccess::txAllowed(backend), "the transmit gate is open");
        backend.setTune(true, 10, authority.operation, {});
        check(sawTuneRequest(backend),
              "gate OPEN with the gateware ATU declared: the tune request does reach "
              "the wire — the gate narrows this feature, it does not disable it");
        backend.setTune(false, 10, authority.operation, {});
    }

    // The CLEARING direction needs no case of its own: applyAtuTuneRequest()
    // computes `tuning && m_hw.atuGateware && m_txAllowed`, so a false `tuning`
    // clears the bit whatever the gate says. That ordering is deliberate — a
    // session that may not transmit must still be able to take a standing
    // request down — and it is a property of the expression, not of a schedule.
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    TxTestAuthority authority;
    MetisClient client;

    testAtuTuneHonoursTheTransmitGate(authority);

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

    // ---- TX IQ FIFO fault accounting (S3 row 3.3) ----
    //
    // The FIFO is bounded above at kTxQueueMax and not below. Both ends change
    // what goes on the air and, until these counters, neither was recorded
    // anywhere: not a log line, not a reading, and not a test. The radio's own
    // DSIQ telemetry cannot stand in for them -- an underflow HERE still emits
    // a full-size EP2 frame on schedule, so the gateware's fill and
    // pacingFault readings are identical whether it happened or not.
    //
    // Every counting assertion below is paired with a NEGATIVE CONTROL on the
    // same counter, because a counter that only ever goes up is a number nobody
    // can act on.
    //
    // BUT BE PRECISE ABOUT WHAT THESE COUNTERS CAN ANSWER, because the obvious
    // reading is wrong and the next two blocks are what establish it: they do
    // NOT answer "was this over clean". A perfectly clean over reads as a
    // BURST of whole-packet underflows followed, an over later, by one more
    // short packet -- and neither end is a fault.
    //
    // THE FLOOR HAS TWO HALVES and the first published account of it named only
    // the smaller one. They are measured separately below:
    //
    //   TAIL, at key-up. Real audio does not end on a packet boundary, so the
    //   last packet is short. ONE packet, 1..kTxSamplesPerPacket-1 samples.
    //
    //   PRE-ROLL, at key-down, and it is the BIGGER half by an order of
    //   magnitude. Nothing primes the queue before MOX goes out, so every EP2
    //   tick between the key edge and the arrival of the transmit chain's FIRST
    //   block takes the empty-queue arm and counts a WHOLE packet.
    //
    // What they answer, therefore, is "how much substituted silence has this
    // PROCESS put on the air" -- a budget, against a floor that is a fixed cost
    // per over rather than a per-over verdict.
    {
        // ---- THE FLOOR, TAIL HALF: what the END of a clean over reads. Measured. ----
        //
        // THIS BLOCK USED TO BE A CONTROL THAT COULD NOT FAIL. It queued an
        // exact multiple of kTxSamplesPerPacket and asserted that a clean
        // transmission counts nothing -- which is to say it probed the one
        // input on which the short-packet branch is never reached, and so the
        // condition it existed to detect could not occur. Swapping the input
        // for a realistic over and leaving the assertion alone turned it red
        // immediately. A control that only ever passes has told nobody
        // anything, and this one was claiming the strongest property in the
        // file.
        //
        // Real audio does not end on a packet boundary. 500 samples is
        // 3*126 + 122, so the LAST packet of a perfectly healthy over is short
        // by 4 and IS COUNTED. Nothing went wrong: the envelope was ending, and
        // four samples of silence before key-up cost nothing on the air. The
        // counter cannot tell that from a starvation because at the moment it
        // counts, "the queue is short" and "the over is over" look identical.
        //
        // So the floor is about ONE underflow packet per over, with between 1
        // and kTxSamplesPerPacket-1 underflow samples riding along. Pinned here
        // rather than described, so that giving the client an end-of-over
        // exemption later is a change that makes THIS assertion fail and say
        // so. That exemption is a design change -- it needs the count deferred
        // and attributed against key-up -- and is deliberately not made here.
        //
        // txUnderflowPackets is the more polluted of the two: its floor is a
        // fixed +1 per over regardless of length, while the tail contributes at
        // most 125 samples against the 126-per-packet a real starvation adds.
        TxTestAuthority tx;
        MetisClient over;
        over.enableTransmit(true);
        over.setMox(true, tx.operation);
        constexpr std::size_t kOverSamples = 500;   // 3 * 126 + 122
        const std::vector<std::complex<float>> speech(
            kOverSamples, std::complex<float>(0.25f, -0.25f));
        over.queueTxIq(speech, tx.context);
        Ep2IqShape lastShape;
        int packets = 0;
        while (over.txQueueDepth() > 0) {
            const auto pkt = over.buildNextControlPacket();
            check(payloadNonZero(pkt), "every packet of a fed over carries IQ");
            lastShape = ep2IqShape(pkt);
            ++packets;
        }
        check(packets == 4, "500 samples is four EP2 packets, the last one short");
        check(over.txUnderflowPackets() == 1,
              "THE FLOOR (TAIL HALF): a CLEAN over that does not end on a packet boundary "
              "still counts one underflow -- this counter cannot report 'this over was clean'");
        check(over.txUnderflowSamples()
                  == static_cast<std::uint64_t>(kTxSamplesPerPacket)
                         - kOverSamples % static_cast<std::size_t>(kTxSamplesPerPacket),
              "and the floor in samples is the packet remainder, here 4");
        // AND THE SAME FACT READ OFF THE WIRE rather than off the counter, so
        // this is not the test recomputing what the code just computed: the
        // final packet really does carry 122 slots of IQ and then falls silent.
        check(lastShape.carryingIq == 122 && lastShape.firstSilent == 122
                  && lastShape.sampleSlots == kTxSamplesPerPacket,
              "the tail packet's WIRE shape is 122 IQ slots then trailing silence");
        check(over.txOverflowSamples() == 0,
              "NEGATIVE CONTROL: a queue that never reached kTxQueueMax counts no overflow");
        std::fprintf(stderr,
            "PROBE  key-up tail: %llu samples (%llu = %d*%d + %llu), drained in %d packets ->"
            " underflowPackets=%llu underflowSamples=%llu\n",
            static_cast<unsigned long long>(kOverSamples),
            static_cast<unsigned long long>(kOverSamples),
            static_cast<int>(kOverSamples / kTxSamplesPerPacket), kTxSamplesPerPacket,
            static_cast<unsigned long long>(kOverSamples % kTxSamplesPerPacket),
            packets,
            static_cast<unsigned long long>(over.txUnderflowPackets()),
            static_cast<unsigned long long>(over.txUnderflowSamples()));
    }

    {
        // ---- THE FLOOR, PRE-ROLL HALF: what the START of a clean over reads.
        //      MEASURED, and it is the larger half. ----
        //
        // Hl2Backend::applyKeying posts setMox to this client immediately. The
        // first block of transmit IQ cannot arrive until the transmit chain has
        // produced one, and Hl2TxDsp::processAudioBlock buffers a whole
        // Config::dspBlockSize block at Config::inputSampleRateHz before it
        // emits anything at all -- it returns early while m_inBuffer is short,
        // and m_inBuffer is empty at key-down twice over (applyKeying posts
        // reset() on unkey, and a new over carries a new TxCoordinator context
        // which makes processAudioBlock reset again).
        //
        // NOTHING PRIMES m_txIq BEFORE MOX. So every EP2 tick in that window
        // takes the empty-queue arm and counts a WHOLE packet, and a clean over
        // OPENS with a burst of them. That is the "missing pre-roll" the
        // accounting comment in MetisClient.h names -- read here as a FLOOR
        // rather than as a fault, because it happens on every healthy
        // transmission and no operator can avoid it.
        //
        // THE COUNT IS DERIVED, NOT TYPED. dspBlockSize and inputSampleRateHz
        // are read off the real Hl2TxDsp::Config, and the EP2 rate off
        // MetisClient's own constant through the friend, so a test that agrees
        // with itself is not possible here: change the DSP block size or the
        // EP2 rate and this figure moves with it.
        const Hl2TxDsp::Config chain{};
        const std::uint64_t preRollPackets =
            (static_cast<std::uint64_t>(chain.dspBlockSize)
                 * static_cast<std::uint64_t>(MetisClientTestAccess::ep2AudioRateHz()))
            / (static_cast<std::uint64_t>(chain.inputSampleRateHz)
                 * static_cast<std::uint64_t>(kTxSamplesPerPacket));
        // PINNED at the geometry this product actually ships: 512 input samples
        // at 24 kHz is 21.33 ms, and an EP2 packet is 126 samples at 48 kHz,
        // which is 2.625 ms. 21.33 / 2.625 = 8.127, so EIGHT whole packets --
        // 1008 samples, about 21 ms -- of substituted silence open every clean
        // over, against a tail of at most 125 samples. If the block size or
        // either rate changes, this assertion is where it says so.
        check(preRollPackets == 8,
              "THE FLOOR (PRE-ROLL HALF): one Hl2TxDsp input block is eight EP2 packets");

        TxTestAuthority tx;
        ScopedTxFifoLog log;
        MetisClient keydown;
        keydown.enableTransmit(true);
        keydown.setMox(true, tx.operation);
        check(keydown.txQueueDepth() == 0,
              "the queue is EMPTY at key-down -- nothing pre-rolls it");
        for (std::uint64_t i = 0; i < preRollPackets; ++i) {
            const auto pkt = keydown.buildNextControlPacket();
            check(anyFrameKeyed(pkt), "the pre-roll packets are KEYED -- this is on the air");
            check(!payloadNonZero(pkt),
                  "every pre-roll packet is a WHOLE packet of substituted silence");
        }
        check(keydown.txUnderflowPackets() == preRollPackets,
              "THE FLOOR (PRE-ROLL HALF): a CLEAN over OPENS with a burst of whole-packet "
              "underflows -- the body's 'roughly one underflow packet per over' was the "
              "tail only, and named the smaller half");
        check(keydown.txUnderflowSamples()
                  == preRollPackets * static_cast<std::uint64_t>(kTxSamplesPerPacket),
              "and in samples the pre-roll is preRollPackets whole packets of silence");
        // WHICH HALF DOMINATES, asserted rather than asserted-in-prose: the
        // pre-roll puts more silence on the air than the largest tail a short
        // packet can possibly contribute.
        check(keydown.txUnderflowSamples()
                  > static_cast<std::uint64_t>(kTxSamplesPerPacket) - 1,
              "the pre-roll outweighs the largest possible tail");
        // AND IT IS ONE EVENT, not eight. The episode is still open here --
        // the queue has not recovered -- so nothing has been logged yet.
        check(log.starvationLines().isEmpty(),
              "an episode still in progress is not logged per packet");
        // PRINTED, so the measured floor is in the run output rather than only
        // in an expectation. This is the figure the body quotes.
        std::fprintf(stderr,
            "PROBE  key-down pre-roll: one Hl2TxDsp block is %d samples at %d Hz"
            " = %.2f ms; an EP2 packet is %d samples at %d Hz = %.3f ms ->"
            " %llu whole-packet underflows, %llu samples of substituted silence,"
            " %.2f ms of every clean over\n",
            chain.dspBlockSize, chain.inputSampleRateHz,
            1000.0 * chain.dspBlockSize / chain.inputSampleRateHz,
            kTxSamplesPerPacket, MetisClientTestAccess::ep2AudioRateHz(),
            1000.0 * kTxSamplesPerPacket / MetisClientTestAccess::ep2AudioRateHz(),
            static_cast<unsigned long long>(keydown.txUnderflowPackets()),
            static_cast<unsigned long long>(keydown.txUnderflowSamples()),
            1000.0 * static_cast<double>(keydown.txUnderflowSamples())
                / MetisClientTestAccess::ep2AudioRateHz());

        // The first Hl2TxDsp block lands and the queue stays primed. Enough
        // for the recovery gap plus room to spare, so the episode closes.
        const std::vector<std::complex<float>> primed(
            static_cast<std::size_t>(kTxSamplesPerPacket)
                * static_cast<std::size_t>(MetisClientTestAccess::runQuietPackets() + 2),
            std::complex<float>(0.25f, -0.25f));
        keydown.queueTxIq(primed, tx.context);
        while (keydown.txQueueDepth() >= static_cast<std::size_t>(kTxSamplesPerPacket))
            (void)keydown.buildNextControlPacket();
        check(keydown.txUnderflowPackets() == preRollPackets,
              "a primed queue adds nothing to the count");
        const QStringList lines = log.starvationLines();
        check(lines.size() == 1,
              "ONE log line for the whole key-down burst, not one per EP2 packet");
        check(!lines.isEmpty()
                  && lines.first().contains(QString::number(preRollPackets)),
              "and the line names the length of the episode it is reporting");
        check(!lines.isEmpty()
                  && lines.first().contains(QStringLiteral("SINCE PROCESS START")),
              "the totals are labelled for what they are -- nothing resets them, "
              "not start(), not a link edge, not flushTxIq()");
    }

    {
        // ---- THE DRIFT SHAPE IS ONE EPISODE, NOT ONE LINE PER PAIR ----
        //
        // The other shape this FIFO produces: the EP2 wall clock and the audio
        // device clock pull apart, so short and full packets ALTERNATE. The
        // first version of the coalescing flushed the run on the next full
        // packet, which turns that into one ~400-character line per PAIR -- up
        // to ~190 a second at the pacer's ~381 frames/s, which is the
        // per-frame flood the coalescing exists to prevent.
        //
        // This is the assertion that makes reportTxUnderflowRun() observable at
        // all. Empty the function and it goes red; before this block, emptying
        // it left every check in the file green.
        TxTestAuthority tx;
        ScopedTxFifoLog log;
        MetisClient drift;
        drift.enableTransmit(true);
        drift.setMox(true, tx.operation);
        constexpr int kPairs = 10;
        constexpr std::size_t kShort = 40;
        for (int i = 0; i < kPairs; ++i) {
            const std::vector<std::complex<float>> ragged(kShort,
                std::complex<float>(0.25f, -0.25f));
            drift.queueTxIq(ragged, tx.context);
            (void)drift.buildNextControlPacket();          // short -> counted
            const std::vector<std::complex<float>> full(
                static_cast<std::size_t>(kTxSamplesPerPacket),
                std::complex<float>(0.25f, -0.25f));
            drift.queueTxIq(full, tx.context);
            (void)drift.buildNextControlPacket();          // full  -> a gap, not a flush
        }
        check(drift.txUnderflowPackets() == kPairs,
              "the drift shape counts one underflow per short packet");
        check(log.starvationLines().isEmpty(),
              "a SINGLE full packet does not end an episode -- alternating short/full "
              "must not log once per pair");

        // The queue keeps up for the recovery gap, so the episode closes -- once.
        const std::vector<std::complex<float>> recovered(
            static_cast<std::size_t>(kTxSamplesPerPacket)
                * static_cast<std::size_t>(MetisClientTestAccess::runQuietPackets() + 1),
            std::complex<float>(0.25f, -0.25f));
        drift.queueTxIq(recovered, tx.context);
        while (drift.txQueueDepth() >= static_cast<std::size_t>(kTxSamplesPerPacket))
            (void)drift.buildNextControlPacket();
        check(log.starvationLines().size() == 1,
              "ten short packets spread across twenty are ONE episode and ONE line");
    }

    {
        // ---- EVERY EXIT FROM THE QUEUED-IQ PATH REPORTS, INCLUDING THE TWO
        //      THAT DID NOT ----
        //
        // MetisClient.h claims the run is reported at every exit. The unkey
        // backstop and flushTxIq() did that; the CW and test-tone arms did not,
        // so an episode in progress when the operator hit TUNE while keyed sat
        // open until key-up and was then timestamped against the wrong moment.
        constexpr std::size_t kShort = 40;

        {   // TUNE taken mid-over
            TxTestAuthority tx;
            ScopedTxFifoLog log;
            MetisClient tune;
            tune.enableTransmit(true);
            tune.setMox(true, tx.operation);
            const std::vector<std::complex<float>> ragged(kShort,
                std::complex<float>(0.25f, -0.25f));
            tune.queueTxIq(ragged, tx.context);
            (void)tune.buildNextControlPacket();     // short -> episode open
            check(log.starvationLines().isEmpty(), "episode open, nothing logged yet");
            tune.setTxTestTone(0.0, 0.5, tx.operation);
            (void)tune.buildNextControlPacket();     // the TONE arm
            check(log.starvationLines().size() == 1,
                  "TUNE while keyed ends the episode WHERE IT HAPPENED, not at key-up");
        }

        {   // the key going up
            TxTestAuthority tx;
            ScopedTxFifoLog log;
            MetisClient unkey;
            unkey.enableTransmit(true);
            unkey.setMox(true, tx.operation);
            const std::vector<std::complex<float>> ragged(kShort,
                std::complex<float>(0.25f, -0.25f));
            unkey.queueTxIq(ragged, tx.context);
            (void)unkey.buildNextControlPacket();
            check(log.starvationLines().isEmpty(), "episode open, nothing logged yet");
            unkey.flushTxIq();                       // what applyKeying posts on unkey
            check(log.starvationLines().size() == 1,
                  "the flush that ends an over also ends the episode");
        }
    }

    {
        // ---- A NaN TONE AMPLITUDE MUST NOT DIVERT QUEUED AUDIO INTO THE
        //      UNKEYED ARM ----
        //
        // setTxTestTone's clamp is
        //     amplitude < 0.0 ? 0.0 : (amplitude > 1.0 ? 1.0 : amplitude)
        // and BOTH comparisons are false for NaN, so NaN reaches m_toneAmp
        // intact. The queued-IQ arm used to read
        //     keyed && !m_cwMode && m_toneAmp <= 0.0
        // which NaN also fails -- as does the tone arm's m_toneAmp > 0.0. A
        // keyed client with a full block of audio waiting therefore fell past
        // BOTH into the UNKEYED backstop and transmitted a whole packet of
        // silence with nothing counted: the exact fault these counters exist to
        // record, reintroduced by the guard written to describe it. The arm is
        // now just `keyed`, which is exact because the two arms above already
        // consume every keyed input with CW or a positive tone amplitude.
        TxTestAuthority tx;
        MetisClient nan;
        nan.enableTransmit(true);
        nan.setMox(true, tx.operation);
        nan.setTxTestTone(0.0, std::numeric_limits<double>::quiet_NaN(), tx.operation);
        const std::vector<std::complex<float>> block(
            static_cast<std::size_t>(kTxSamplesPerPacket),
            std::complex<float>(0.25f, -0.25f));
        nan.queueTxIq(block, tx.context);
        const auto pkt = nan.buildNextControlPacket();
        check(anyFrameKeyed(pkt), "the client is keyed");
        check(payloadNonZero(pkt),
              "a NaN tone amplitude must not send queued audio to the unkeyed arm -- "
              "that transmitted silence UNCOUNTED with a full block waiting");
        check(nan.txQueueDepth() == 0,
              "the queued block was consumed by the queued-IQ arm, as for any other amplitude");
        check(nan.txUnderflowPackets() == 0,
              "NEGATIVE CONTROL: a full block under a NaN amplitude is not an underflow");
    }

    {
        // ---- NEGATIVE CONTROL: the packet boundary, narrowly ----
        //
        // An exact multiple of kTxSamplesPerPacket drains with every packet
        // full, so the short-packet branch is never taken. Retained, but it is
        // presented for what it is -- the ARITHMETIC edge -- and NOT as "a
        // clean transmission counts nothing", which the block above shows to be
        // false. What it pins is that a FULL packet is never counted, and that
        // is a real guard: it is what kills the mutation "count on full packets
        // as well as short ones" (n <= where the code has n <).
        TxTestAuthority tx;
        MetisClient clean;
        clean.enableTransmit(true);
        clean.setMox(true, tx.operation);
        const std::vector<std::complex<float>> exact(
            static_cast<std::size_t>(kTxSamplesPerPacket) * 4, std::complex<float>(0.25f, -0.25f));
        clean.queueTxIq(exact, tx.context);
        for (int i = 0; i < 4; ++i) {
            const auto pkt = clean.buildNextControlPacket();
            check(payloadNonZero(pkt), "a fully fed keyed packet carries IQ");
            const auto shape = ep2IqShape(pkt);
            check(shape.carryingIq == kTxSamplesPerPacket && shape.firstSilent < 0,
                  "a full packet has NO silent slot -- nothing was substituted");
        }
        check(clean.txQueueDepth() == 0, "four packets drain four packets' worth");
        check(clean.txUnderflowPackets() == 0 && clean.txUnderflowSamples() == 0,
              "NEGATIVE CONTROL: a queue drained on an exact packet boundary counts no underflow");
    }

    {
        // ---- NEGATIVE CONTROL: unkeyed silence is not a fault ----
        //
        // An unkeyed frame carries transmit silence BY DESIGN. Counting it
        // would make the counter read a fault on a radio that is receiving,
        // which is the failure mode the resting state of the gateware's own
        // txFifoRecovery flag already demonstrates: a reading with a non-zero
        // idle baseline reports idle as fault.
        TxTestAuthority tx;
        MetisClient idle;
        idle.enableTransmit(true);          // gate OPEN, key UP
        for (int i = 0; i < 16; ++i) {
            const auto pkt = idle.buildNextControlPacket();
            check(!payloadNonZero(pkt), "an unkeyed frame carries silence");
        }
        check(idle.txUnderflowPackets() == 0 && idle.txUnderflowSamples() == 0,
              "NEGATIVE CONTROL: unkeyed silence is the design, not an underflow");
    }

    {
        // ---- POSITIVE: a PARTIAL block is counted, in samples of silence ----
        //
        // This is the drift case: the audio device clock and the EP2 wall
        // clock have pulled apart far enough that the queue holds less than
        // one packet. FIND-23 measured exactly this shape on EP2 captures of
        // live speech -- one starvation per over, of 18 to 64 samples -- and
        // measured what it costs: windows with no starvation give 78.6-78.8 dB
        // opposite-sideband suppression, windows with one give 33.7-35.2 dB.
        // Those captures ran against hpsdrsim on loopback, not a radio, so the
        // delta is what they establish and not the on-air magnitude. Nothing in
        // THIS test depends on either figure; the assertions below are on the
        // counters.
        TxTestAuthority tx;
        MetisClient partial;
        partial.enableTransmit(true);
        partial.setMox(true, tx.operation);
        constexpr std::size_t kShort = 40;
        const std::vector<std::complex<float>> ragged(
            static_cast<std::size_t>(kTxSamplesPerPacket) + kShort,
            std::complex<float>(0.25f, -0.25f));
        partial.queueTxIq(ragged, tx.context);

        (void)partial.buildNextControlPacket();   // the full one
        check(partial.txUnderflowPackets() == 0,
              "the full packet ahead of the short one is not counted");

        const auto shortPkt = partial.buildNextControlPacket();   // the short one
        check(partial.txUnderflowPackets() == 1, "one short packet, one underflow packet");
        check(partial.txUnderflowSamples()
                  == static_cast<std::uint64_t>(kTxSamplesPerPacket) - kShort,
              "underflow counts the SILENCE substituted, not the samples supplied");
        // THE ASSERTION WITH TEETH. The line above computes
        // kTxSamplesPerPacket - kShort and so does buildNextControlPacket, from
        // the same input -- both sides agreeing proves only that the arithmetic
        // was performed twice. This one reads the OTHER thing: the packet that
        // actually went out carries exactly kShort slots of IQ and is silent
        // from there to the end, which is the trailing-zero signature FIND-23
        // measured and the observable the counter stands in for.
        const auto shortShape = ep2IqShape(shortPkt);
        check(shortShape.carryingIq == static_cast<int>(kShort)
                  && shortShape.firstSilent == static_cast<int>(kShort),
              "the short packet's WIRE shape is kShort IQ slots then trailing silence");
    }

    {
        // ---- POSITIVE: an EMPTY queue is the LARGEST underflow, not a no-op ----
        //
        // Before the counters this case did not even reach the queued-IQ
        // branch -- `keyed && !m_txIq.empty()` was false, the chain fell off
        // the end, and a whole packet of silence went on the air recorded by
        // nothing. It is the missing pre-roll: at key-down the queue has not
        // been primed, so the first packets of every over take this path.
        //
        // The wire is asserted UNCHANGED here as well. These counters observe;
        // they must not alter a single byte of what the radio receives.
        TxTestAuthority tx;
        MetisClient starved;
        starved.enableTransmit(true);
        starved.setMox(true, tx.operation);
        check(starved.txQueueDepth() == 0, "nothing queued");
        const auto pkt = starved.buildNextControlPacket();
        check(!payloadNonZero(pkt),
              "an empty keyed queue still emits a byte-identical silent EP2 payload");
        check(starved.txUnderflowPackets() == 1
                  && starved.txUnderflowSamples() == static_cast<std::uint64_t>(kTxSamplesPerPacket),
              "an empty queue counts a WHOLE packet of substituted silence");
    }

    {
        // ---- POSITIVE + NEGATIVE: overflow, against the real bound ----
        //
        // Overflow drops the OLDEST, which is the right policy on transmit and
        // is also a discontinuity in the middle of an envelope. Counted for
        // the same reason as the underflow: nothing else records it.
        TxTestAuthority tx;
        MetisClient over;
        constexpr std::size_t kPast = 500;
        const std::vector<std::complex<float>> flood(
            MetisClientTestAccess::queueMax() + kPast, std::complex<float>(0.1f, 0.1f));
        over.queueTxIq(flood, tx.context);
        check(over.txQueueDepth() == MetisClientTestAccess::queueMax(),
              "the queue is bounded above at kTxQueueMax");
        check(over.txOverflowSamples() == kPast,
              "overflow counts exactly the samples dropped past the bound");
        check(over.txUnderflowPackets() == 0,
              "NEGATIVE CONTROL: overflowing the queue is not also an underflow");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_tx_gate_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
