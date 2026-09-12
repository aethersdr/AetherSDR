// RQST/ACK where it meets the wire: MetisClient's own EP2 packet builder and
// its EP6 response path. Socket-free — no bind, no peer, no datagrams, no event
// loop — using the same builder the transport calls and the same response
// struct parseEp6Response produces.
//
// The state machine's own laws are tested in hl2_rqst_ack_test. What is tested
// HERE is everything that only exists once the machine is attached to a radio:
// that the request reaches the wire exactly once, that it cannot key a
// transmitter, that it cannot be issued at a transmit-capable register, that it
// cannot be issued at all before there is a stream to answer on, and that it
// does not overtake a write the operator asked for.

#include "core/backends/hl2/Hl2ControlRequest.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>

#include <array>
#include <cstdio>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    // A client that believes it is streaming and has seen EP6, without either
    // having happened. Both flags matter: requestRegister() refuses before the
    // stream is up, because response slots live inside the EP6 frame.
    static void setStreaming(MetisClient& c) { c.m_running = true; c.m_linkUp = true; }
    // Drive the reply path with a synthetic response, the way a datagram would.
    static void feedResponse(MetisClient& c, const Ep6Response& r)
    {
        c.ingestControlResponse(r);
    }
    // One EP6 frame's worth of deadline, the way a datagram would, on the
    // caller's clock. The clock is a parameter all the way down: a frame is not
    // a fixed amount of time, so the deadline has a wall-clock floor as well as
    // a frame count, and this test can move both independently.
    static void feedFrame(MetisClient& c, qint64 nowMs) { c.tickControlRequest(nowMs); }
    // Confirm the packet just built reached the socket, the way
    // sendControlPacket() does with sendTo()'s return value. `bytes <= 0` is a
    // write the kernel refused.
    static void confirmSent(MetisClient& c, qint64 bytes, qint64 nowMs)
    {
        c.onControlPacketSent(bytes, nowMs);
    }
};
}

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

using Packet = std::array<std::uint8_t, kUsbPacketSize>;
static constexpr std::size_t kFrameA = 8;
static constexpr std::size_t kFrameB = 8 + kFrameSize;

static std::uint8_t c0(const Packet& p, std::size_t frame) { return p[frame + 3]; }

static bool anyFrameKeyed(const Packet& p)
{
    return (c0(p, kFrameA) & kC0MoxBit) != 0 || (c0(p, kFrameB) & kC0MoxBit) != 0;
}
static bool anyFrameRequests(const Packet& p)
{
    return (c0(p, kFrameA) & kC0RespRqstBit) != 0 || (c0(p, kFrameB) & kC0RespRqstBit) != 0;
}
static std::uint32_t dataOf(const Packet& p, std::size_t frame)
{
    return (std::uint32_t(p[frame + 4]) << 24) | (std::uint32_t(p[frame + 5]) << 16)
         | (std::uint32_t(p[frame + 6]) << 8) | std::uint32_t(p[frame + 7]);
}

// Build a packet AND confirm it reached the socket — the two-step the transport
// performs. Split on purpose: the deadline must not start on a bank that only
// got as far as a buffer, so every test that means "this went out" says both.
// The fake wall clock, in milliseconds. Every test that cares about the
// deadline drives it explicitly; the rest leave it at zero, which is correct
// because a request that is answered never consults the floor.
static qint64 g_nowMs = 0;

static Packet sendPacket(MetisClient& c)
{
    const auto pkt = c.buildNextControlPacket();
    MetisClientTestAccess::confirmSent(c, static_cast<qint64>(kUsbPacketSize), g_nowMs);
    return pkt;
}

static Ep6Response ack(int raddr, std::uint32_t data)
{
    Ep6Response r;
    r.ack = true;
    r.raddr = raddr;
    r.data = data;
    return r;
}

// ---------------------------------------------------------------------------

static void testRefusedBeforeThereIsAStream()
{
    MetisClient c;
    // The EP6 emit path is gated on `run` (usopenhpsdr1.v). An idle radio
    // answers discovery and nothing else, for ever — so arming here would
    // guarantee a timeout and blame the hardware for our own ordering.
    check(!c.requestRegister(0x0E, 0), "a request before the stream is up is REFUSED");
    check(c.controlRequest().state() == Hl2ControlRequest::State::Idle,
          "and arms nothing");
    for (int i = 0; i < 16; ++i)
        check(!anyFrameRequests(sendPacket(c)),
              "no RQST bit reaches the wire from a refused request");
}

static void testOnlyTheAllowListedAddressesAreRequestable()
{
    MetisClient c;
    MetisClientTestAccess::setStreaming(c);

    // The whole of the allow-list, and each one has to be re-armable after the
    // previous answer or the loop below would prove nothing after the first.
    const int allowed[] = {kC0AdcGain >> 1, kC0AdcAssignOrTxGain >> 1};
    for (const int a : allowed) {
        check(c.requestRegister(a, 0x40u), "an allow-listed address is accepted");
        while (c.controlRequest().state() == Hl2ControlRequest::State::Queued)
            (void)sendPacket(c);
        MetisClientTestAccess::feedResponse(c, ack(a, 0x40u));
        check(c.controlRequest().state() == Hl2ControlRequest::State::Idle,
              "and settles, so the next one can be armed");
    }

    // EVERY other six-bit address is refused, which is the property a deny-list
    // could not have: this passes for addresses nobody has thought about yet.
    // Named individually below are the ones with a reason worth stating.
    for (int a = 0; a < 0x40; ++a) {
        const bool onList = a == (kC0AdcGain >> 1) || a == (kC0AdcAssignOrTxGain >> 1);
        if (onList)
            continue;
        check(!c.requestRegister(a, 0), "an address not on the allow-list is refused");
    }

    // 0x3d is the external companion bus — a Pico that switches amplifiers,
    // antenna relays and transverters. 0x3c is the internal bus (Versa clock,
    // AD9866). Neither self-corrects, and neither is on the list.
    check(!c.requestRegister(kC0I2c2 >> 1, 0), "0x3d (I2C2, companion board) is refused");
    check(!c.requestRegister(kC0I2c1 >> 1, 0), "0x3c (I2C1, internal bus) is refused");
    // 0x01 is the TX NCO, and unlike 0x00/0x02..0x08 the round robin never
    // re-asserts it: a bad write there would persist until the next tune.
    check(!c.requestRegister(kC0TxFreq >> 1, 0), "0x01 (TX NCO, not re-asserted) is refused");
    // 0x09 is TX drive level, onboard PA enable and ATU: its DATA is what puts
    // RF out of the socket. 0x39 is sync/reset, which carries the watchdog and
    // master enables and has wedged a radio.
    check(!c.requestRegister(kC0TxDrive >> 1, 0), "0x09 (TX drive / PA) is refused");
    check(!c.requestRegister(kC0Sync >> 1, 0), "0x39 (sync / reset) is refused");
    // 0x3b is off the list too, and this is the case the reviewer found. It was
    // admitted as "the subsystem read path", which ad9866ctrl.v contradicts: the
    // module has no data output at all, and control.v's AD9866 branch of
    // RESP_READ assigns the I2C bus's data behind its own
    // "// FIXME: suppor read cmd_resp_data_ad9866". What it IS, gated on
    // cmd_data[31:24] == 8'h06, is a generic converter SPI WRITE of
    // {3'b000, cmd_data[20:16], cmd_data[7:0]} — an arbitrary AD9866 register,
    // including 0x0a, where the gateware's own TX-gain command writes
    // (icmd_data = {5'h0a,4'b0100,tx_gain}). And it persists harder than 0x01
    // does: the 0x09 handler re-writes gain only `if (tx_gain != cmd_data[31:28])`
    // against an FPGA shadow a 0x3b write never touches, so the mechanism that
    // would correct it is suppressed by its own change detector. By this list's
    // own re-asserted-or-excluded rule, that is an exclusion.
    check(!c.requestRegister(kC0Ad9866Spi >> 1, 0),
          "0x3b (raw AD9866 SPI write, reaches the TX gain register) is refused");
    check(!c.requestRegister(kC0Ad9866Spi >> 1, 0x060A004Fu),
          "including with the 8'h06 cookie the gateware actually acts on");
    check(!c.requestRegister(kC0Ad9866Spi >> 1, 0x00ABCD12u, true),
          "and as a claimed subsystem read");
    // Refused even with the transmit gate explicitly open: this layer is not
    // the place that decision gets made, and a future writer must add the
    // conditional deliberately rather than find it already gone.
    c.enableTransmit(true);
    check(!c.requestRegister(kC0TxDrive >> 1, 0), "0x09 stays refused with the gate OPEN");
    check(!c.requestRegister(kC0Sync >> 1, 0), "0x39 stays refused with the gate OPEN");
    check(!c.requestRegister(kRespAddrError, 0), "0x3F is refused");
    // No allow-listed address replies with a READ value — on this gateware only
    // the I2C buses do, and neither is reachable — so asking for
    // Echo::SubsystemRead is asking to discard the echo and match on six bits of
    // address alone. That is the pairing the quarantine narrows but cannot rule
    // out, so it is refused rather than silently downgraded.
    check(!c.requestRegister(kC0AdcGain >> 1, 0x40u, true),
          "a claimed subsystem read at 0x0a is REFUSED, not downgraded");
    check(!c.requestRegister(kC0AdcAssignOrTxGain >> 1, 0x40u, true),
          "and at 0x0e");
    check(c.controlRequest().state() == Hl2ControlRequest::State::Idle,
          "and none of those refusals armed anything");
    check(!c.requestRegister(0x40, 0), "an address past the six-bit field is refused");
    check(!c.requestRegister(-1, 0), "a negative address is refused");
}

static void testAFailedSendDoesNotBurnTheRequest()
{
    // onRequestSent() runs on the socket's return value, not on the build. A
    // write the kernel refused must leave the request Queued so it goes out on
    // the next EP2 frame — starting the deadline here would spend 32 frames
    // plus 32 of quarantine on a command the radio was never shown, and report
    // it as "the radio did not answer".
    MetisClient c;
    MetisClientTestAccess::setStreaming(c);
    check(c.requestRegister(0x0E, 0xDEAD'BEEFu), "armed");

    const auto lost = c.buildNextControlPacket();
    check(anyFrameRequests(lost), "the RQST bank was built");
    MetisClientTestAccess::confirmSent(c, -1, g_nowMs);          // sendTo() failed
    check(c.controlRequest().state() == Hl2ControlRequest::State::Queued,
          "a failed send leaves the request QUEUED, deadline not started");

    const auto retry = sendPacket(c);
    check(anyFrameRequests(retry), "and it goes out on the next frame instead");
    check(dataOf(retry, kFrameB) == 0xDEAD'BEEFu, "with the same data");
    check(c.controlRequest().state() == Hl2ControlRequest::State::Awaiting,
          "only a send that succeeded starts the deadline");
    // Still exactly once overall: the lost packet is not a second command.
    int further = 0;
    for (int i = 0; i < 32; ++i)
        if (anyFrameRequests(sendPacket(c)))
            ++further;
    check(further == 0, "and never a third time");
}

static void testLinkDownDropsTheRequestAndSaysSo()
{
    // reset()'s own doc is "for a link that went down". stop() did it; the
    // silence watchdog did not, so a metis-stop/restart left a stale Awaiting
    // that later reported TimedOut for a request the radio may have applied.
    // Both now go through dropControlRequest(), which is what this exercises —
    // via stop(), because the watchdog needs a socket and a timer and this test
    // has neither by design.
    MetisClient c;
    MetisClientTestAccess::setStreaming(c);
    int failures = 0, lastAddr = -1;
    bool lastRefused = true;
    QObject::connect(&c, &MetisClient::controlRequestFailed,
                     [&](int a, bool refused) { ++failures; lastAddr = a; lastRefused = refused; });

    check(c.requestRegister(0x0E, 7), "armed");
    while (c.controlRequest().state() == Hl2ControlRequest::State::Queued)
        (void)sendPacket(c);
    c.stop();
    check(c.controlRequest().state() == Hl2ControlRequest::State::Idle,
          "the link going down forgets the outstanding request");
    check(failures == 1 && lastAddr == 0x0E && !lastRefused,
          "and the caller is told, rather than left waiting for a signal that "
          "can no longer be emitted");
}

static void testRequestReachesTheWireExactlyOnce()
{
    MetisClient c;
    MetisClientTestAccess::setStreaming(c);
    check(c.requestRegister(0x0E, 0x1234'5678u), "armed");

    int seen = 0;
    Packet requestPacket{};
    // Well past a full round robin (numRx + 2 slots) several times over. A RQST
    // bit that ended up latched into a rotating bank would re-request for ever,
    // and every repeat would be a fresh command the radio's one-deep response
    // register has to serve.
    for (int i = 0; i < 64; ++i) {
        const auto pkt = sendPacket(c);
        if (anyFrameRequests(pkt)) { ++seen; requestPacket = pkt; }
        check(!anyFrameKeyed(pkt), "no frame is ever keyed by a request");
    }
    check(seen == 1, "the RQST bit appears on the wire EXACTLY once");

    // The config bank rides frame A of every packet; the request is a one-shot
    // and lands in frame B.
    check((c0(requestPacket, kFrameA) & kC0RespRqstBit) == 0,
          "the standing config bank never carries RQST");
    const std::uint8_t rc0 = c0(requestPacket, kFrameB);
    check((rc0 & kC0RespRqstBit) != 0, "frame B carries RQST");
    check((rc0 & kC0MoxBit) == 0, "and not MOX");
    check(((rc0 >> 1) & kMaxRegisterAddress) == 0x0E, "at the requested address");
    check(dataOf(requestPacket, kFrameB) == 0x1234'5678u, "with the requested data");

    check(c.controlRequest().state() == Hl2ControlRequest::State::Awaiting,
          "sending the bank starts the deadline");
}

static void testARequestCannotKeyAKeyedRadioAnyHarder()
{
    // With the transmit gate CLOSED and a key request standing, the existing
    // invariant (hl2_tx_gate_test) is that no frame carries C0 bit 0. A request
    // bank is a new kind of frame, so it is checked against the same law.
    MetisClient c;
    MetisClientTestAccess::setStreaming(c);
    c.setMox(true);                                   // refused: gate closed
    check(!c.isKeyed(), "the gate is closed");
    check(c.requestRegister(0x0E, 0), "armed");
    for (int i = 0; i < 32; ++i) {
        c.setMox(true);
        check(!anyFrameKeyed(sendPacket(c)),
              "a request frame is not a way past the transmit gate");
    }
}

static void testRequestDoesNotOvertakeAnOperatorWrite()
{
    // A one-shot is a write the operator asked for. A read-back that overtook
    // it would answer with the value from before the change — and look correct.
    MetisClient c;
    MetisClientTestAccess::setStreaming(c);
    // An IO-board band push: five one-shot banks at a register nothing else in
    // this client writes, so "did the write go first" is unambiguous.
    c.setIoBoardTxFrequencyHz(7'100'000);
    check(c.requestRegister(kC0AdcGain >> 1, 0x40u), "armed behind the one-shots");

    int boardWritesBeforeRequest = 0;
    bool sawRequest = false;
    for (int i = 0; i < 16 && !sawRequest; ++i) {
        const auto pkt = sendPacket(c);
        const std::uint8_t b = c0(pkt, kFrameB);
        if ((b & kC0RespRqstBit) != 0)
            sawRequest = true;
        else if ((b & ~kC0MoxBit) == kC0I2c2)
            ++boardWritesBeforeRequest;
    }
    check(sawRequest, "the request did go out");
    check(boardWritesBeforeRequest == 5,
          "every bank of the operator's write goes out BEFORE the read-back");
}

static void testReplyAndTimeoutReachTheSeam()
{
    MetisClient c;
    MetisClientTestAccess::setStreaming(c);

    int replies = 0, failures = 0, lastAddr = -1;
    std::uint32_t lastData = 0;
    bool lastRefused = false;
    QObject::connect(&c, &MetisClient::controlReplyReady,
                     [&](int a, quint32 d) { ++replies; lastAddr = a; lastData = d; });
    QObject::connect(&c, &MetisClient::controlRequestFailed,
                     [&](int a, bool refused) { ++failures; lastAddr = a; lastRefused = refused; });

    // --- answered ---
    check(c.requestRegister(0x0E, 0x0000'8000u), "armed");
    while (c.controlRequest().state() == Hl2ControlRequest::State::Queued)
        (void)sendPacket(c);                           // put it on the wire
    MetisClientTestAccess::feedResponse(c, ack(0x0E, 0x0000'8000u));
    check(replies == 1, "a matching ACK publishes a reply");
    check(lastAddr == 0x0E && lastData == 0x0000'8000u, "carrying the echo");
    check(failures == 0, "and no failure");

    // --- refused by the radio ---
    check(c.requestRegister(0x0E, 1), "re-armed after an answer");
    while (c.controlRequest().state() == Hl2ControlRequest::State::Queued)
        (void)sendPacket(c);
    MetisClientTestAccess::feedResponse(c, ack(kRespAddrError, 0));
    check(failures == 1 && lastRefused, "a 0x3F reply publishes a refusal");
    check(lastAddr == 0x0E, "named by the address asked for");

    // --- unanswered ---
    check(c.requestRegister(0x0E, 2), "re-armed after a refusal");
    while (c.controlRequest().state() == Hl2ControlRequest::State::Queued)
        (void)sendPacket(c);
    // The frame count alone is no longer a timeout. At 384 kHz with three
    // receivers these 32 frames are 2.08 ms of wall clock, which is inside a
    // single recorded EP6 delivery gap — the radio could not have answered yet.
    for (int i = 0; i < Hl2ControlRequest::kDefaultDeadlineFrames * 4; ++i)
        MetisClientTestAccess::feedFrame(c, g_nowMs);
    check(failures == 1, "frames alone do not publish a timeout any more");
    check(c.controlRequest().state() == Hl2ControlRequest::State::Awaiting,
          "the request is still outstanding while the floor holds");

    // Past the floor, both halves are satisfied and it settles as it always did.
    g_nowMs += Hl2ControlRequest::kDefaultFloorMs;
    MetisClientTestAccess::feedFrame(c, g_nowMs);
    check(failures == 2 && !lastRefused,
          "a deadline with no ACK publishes a timeout, not silence");
    check(replies == 1, "and no reply");
    // And the caller cannot simply retry: the machine is quarantining whatever
    // the radio still owes it.
    check(!c.requestRegister(0x0E, 3), "an immediate retry after a timeout is REFUSED");
    for (int i = 0; i < Hl2ControlRequest::kDefaultQuarantineFrames; ++i)
        MetisClientTestAccess::feedFrame(c, g_nowMs);
    check(!c.requestRegister(0x0E, 3),
          "the quarantine has a wall-clock floor of its own, from the moment we gave up");
    g_nowMs += Hl2ControlRequest::kDefaultFloorMs;
    MetisClientTestAccess::feedFrame(c, g_nowMs);
    check(c.requestRegister(0x0E, 3), "and accepted once BOTH halves have elapsed");
}

static void testStopClearsTheOutstandingRequest()
{
    MetisClient c;
    MetisClientTestAccess::setStreaming(c);
    check(c.requestRegister(0x0E, 1), "armed");
    c.stop();
    check(c.controlRequest().state() == Hl2ControlRequest::State::Idle,
          "stop() forgets the outstanding request");
    // And it does not leak onto the next session's wire.
    MetisClientTestAccess::setStreaming(c);
    for (int i = 0; i < 16; ++i)
        check(!anyFrameRequests(sendPacket(c)),
              "no stale RQST survives a stop");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testRefusedBeforeThereIsAStream();
    testOnlyTheAllowListedAddressesAreRequestable();
    testRequestReachesTheWireExactlyOnce();
    testARequestCannotKeyAKeyedRadioAnyHarder();
    testRequestDoesNotOvertakeAnOperatorWrite();
    testReplyAndTimeoutReachTheSeam();
    testStopClearsTheOutstandingRequest();
    testAFailedSendDoesNotBurnTheRequest();
    testLinkDownDropsTheRequestAndSaysSo();

    if (g_failures == 0)
        std::printf("hl2_rqst_ack_client_test: OK\n");
    return g_failures == 0 ? 0 : 1;
}
