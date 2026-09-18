// The HL2 RQST/ACK state machine (docs/HERMES.md §13 item 13, oracle §5).
//
// What is actually under test is not "a request gets a reply". It is the three
// properties that make this protocol not an RPC, each of which is a way to be
// wrong that LOOKS LIKE SUCCESS:
//
//   - a second request cannot be armed, because the gateware's response
//     register holds one reply and silently drops the loser;
//   - a reply is paired by ECHO, so a reply that is merely plausible must be
//     rejected rather than accepted;
//   - a reply to an abandoned request must have nowhere to land, because with
//     no transaction id it is otherwise indistinguishable from a timely one.
//
// Pure: no Qt, no socket, no radio, and no clock OF ITS OWN. Time is EP6
// frames — the only clock the radio answers on — plus a wall-clock floor that
// is PASSED IN, because a frame is not a fixed amount of time: 32 frames is
// 42 ms at 48 kHz with one receiver and 2.08 ms at 384 kHz with three. The fake
// clock below is what lets this file pin both without real time passing.

#include "core/backends/hl2/Hl2ControlRequest.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <cstdio>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

using State = Hl2ControlRequest::State;
using Outcome = Hl2ControlRequest::Outcome;
using Echo = Hl2ControlRequest::Echo;

// An ACK as the radio composes it: C0 = {1, raddr[5:0], ptt}.
static Ep6Response ack(int raddr, std::uint32_t data)
{
    Ep6Response r;
    r.ack = true;
    r.raddr = raddr;
    r.data = data;
    return r;
}

// A free-running telemetry response: C0 = {000, raddr[1:0], cwkey, 0, ptt}.
static Ep6Response classic(int raddr, std::uint32_t data)
{
    Ep6Response r;
    r.ack = false;
    r.raddr = raddr;
    r.data = data;
    return r;
}

// A fake wall clock in milliseconds, advanced explicitly. The machine reads no
// clock of its own — that is what lets this file pin a 384 kHz configuration
// with no real time passing, which is the whole reason the clock is a
// parameter.
static std::int64_t g_nowMs = 0;

// Run n EP6 frames AT A GIVEN SAMPLE RATE AND RECEIVER COUNT, advancing the
// fake clock by what those frames actually take on the wire. A frame is
// 504/(6*numRx+2) rounds, so this is the geometry from MetisProtocol.h and not
// a guess: at 48 kHz with one receiver 32 frames is 42 ms, and at 384 kHz with
// three it is 2.08 ms.
static void runFramesAtRate(Hl2ControlRequest& m, int n, int sampleRateHz, int numRx)
{
    const double framesPerSecond =
        static_cast<double>(sampleRateHz) / ep6RoundsPerFrame(numRx);
    for (int i = 1; i <= n; ++i) {
        g_nowMs = static_cast<std::int64_t>(
            static_cast<double>(i) * 1000.0 / framesPerSecond);
        m.onEp6Frame(g_nowMs);
    }
}

// Frames with no time passing at all: isolates the frame COUNT from the floor.
static void runFrames(Hl2ControlRequest& m, int n)
{
    for (int i = 0; i < n; ++i)
        m.onEp6Frame(g_nowMs);
}

// ---------------------------------------------------------------------------

static void testAddressSpace()
{
    check(Hl2ControlRequest::isRequestableAddress(0x00), "0x00 is requestable");
    check(Hl2ControlRequest::isRequestableAddress(0x3E), "0x3E is requestable");
    // 0x3F is how the radio SAYS "refused" (control.v RESP_ACK substitutes
    // 6'h3f). Requesting it would make a refusal and an answer the same bytes.
    check(!Hl2ControlRequest::isRequestableAddress(kRespAddrError),
          "0x3F is NOT requestable — it is the refusal encoding");
    // The C0 address field is six bits (dsopenhpsdr1.v: addr <= eth_data[6:1]),
    // so 0x40 is not a bigger address, it is the RQST flag.
    check(!Hl2ControlRequest::isRequestableAddress(0x40),
          "0x40 is NOT requestable — six-bit field");
    check(!Hl2ControlRequest::isRequestableAddress(-1), "negative is not requestable");

    Hl2ControlRequest m;
    check(!m.arm({kRespAddrError, 0, Echo::Exact}), "arm refuses the refusal address");
    check(m.state() == State::Idle, "a refused arm changes nothing");
}

static void testWireBank()
{
    Hl2ControlRequest m;
    check(m.wireBank() == std::nullopt, "Idle offers no bank");
    check(m.arm({0x0E, 0xDEADBEEFu, Echo::Exact}), "arm accepted from Idle");
    check(m.state() == State::Queued, "armed -> Queued");

    const auto bank = m.wireBank();
    check(bank.has_value(), "Queued offers a bank");
    const Cc cc = *bank;
    check((cc[0] & kC0RespRqstBit) != 0, "the RQST bit is SET on the request bank");
    check((cc[0] & kC0MoxBit) == 0, "the request bank NEVER sets MOX");
    check(((cc[0] >> 1) & kMaxRegisterAddress) == 0x0E, "address lands in C0[6:1]");
    check(cc[1] == 0xDE && cc[2] == 0xAD && cc[3] == 0xBE && cc[4] == 0xEF,
          "data is big-endian across C1..C4");

    // Queued does not tick: a request still behind the one-shot queue has not
    // reached the radio, so frames that pass are not frames it failed to answer.
    runFrames(m, 1000);
    check(m.state() == State::Queued, "the deadline does not run before the bank is sent");

    m.onRequestSent(g_nowMs);
    check(m.state() == State::Awaiting, "sent -> Awaiting");
    check(m.wireBank() == std::nullopt,
          "Awaiting offers NO bank — the request goes on the wire exactly once");
}

static void testSingleOutstanding()
{
    Hl2ControlRequest m;
    check(m.arm({0x0E, 1, Echo::Exact}), "first arm accepted");
    check(!m.arm({0x14, 2, Echo::Exact}), "second arm REFUSED while Queued");
    check(m.outstanding().addr == 0x0E, "a refused arm does not displace the outstanding one");
    check(m.outstanding().data == 1, "nor its data");

    m.onRequestSent(g_nowMs);
    check(!m.arm({0x14, 2, Echo::Exact}), "second arm REFUSED while Awaiting");

    check(m.onResponse(ack(0x0E, 1)), "the matching ACK is consumed");
    check(m.state() == State::Settled, "matched -> Settled");
    check(!m.arm({0x14, 2, Echo::Exact}),
          "second arm REFUSED while a verdict is unread — the caller must look");

    const auto reply = m.takeReply();
    check(reply.has_value(), "a verdict is available");
    check(reply->outcome == Outcome::Answered, "outcome is Answered");
    check(reply->addr == 0x0E && reply->data == 1, "the verdict carries the echo");
    check(m.state() == State::Idle, "an answered request frees the slot");
    check(m.takeReply() == std::nullopt, "a verdict is delivered ONCE");
    check(m.arm({0x14, 2, Echo::Exact}), "the slot is reusable after the verdict is read");
    check(m.answered() == 1 && m.staleAcks() == 0, "counters");
}

static void testEchoMatchIsNarrow()
{
    Hl2ControlRequest m;
    check(m.arm({0x0E, 0x0000'1234u, Echo::Exact}), "armed");
    m.onRequestSent(g_nowMs);

    // The reply that is merely PLAUSIBLE. Right register, wrong data: on this
    // protocol that is the reply to the previous write at the same register,
    // and accepting it hands the caller a value it never asked for.
    check(!m.onResponse(ack(0x0E, 0x0000'1233u)), "same address, wrong echo: REJECTED");
    check(m.state() == State::Awaiting, "a rejected reply does not settle anything");
    check(m.staleAcks() == 1, "and is counted as stale");

    check(!m.onResponse(ack(0x14, 0x0000'1234u)), "wrong address, right data: REJECTED");
    check(m.staleAcks() == 2, "counted");

    // The nastiest near-miss of all: the free-running telemetry cycle runs
    // through raddr 0..3 continuously, so at any moment there is a response on
    // the wire whose raddr could equal a low request address. C0[7] is the only
    // thing that separates them.
    check(!m.onResponse(classic(0x0E, 0x0000'1234u)),
          "a non-ACK response never matches, whatever it carries");
    check(m.staleAcks() == 2, "a non-ACK is not even stale — it is telemetry");
    check(m.state() == State::Awaiting, "still outstanding");

    check(m.onResponse(ack(0x0E, 0x0000'1234u)), "the exact echo matches");
    check(m.takeReply()->outcome == Outcome::Answered, "answered");
}

static void testSubsystemReadMatchesOnAddressOnly()
{
    // 0x3c is the I2C command register, and it is the ONLY shape of command
    // whose reply carries the value READ rather than the bytes written
    // (control.v RESP_READ assigns cmd_resp_data_i2c). Demanding an echo here
    // would reject every successful read.
    //
    // NOT 0x3b. The AD9866 SPI command was described as a read path and is not
    // one: ad9866ctrl has no data output, and control.v's AD9866 branch of
    // RESP_READ assigns the I2C bus's data behind its own
    // "// FIXME: suppor read cmd_resp_data_ad9866". The mode stays here because
    // the I2C buses will need it; MetisClient::requestRegister reaches neither
    // and refuses the flag outright, which hl2_rqst_ack_client_test pins.
    Hl2ControlRequest m;
    check(m.arm({0x3C, 0x07EA0000u, Echo::SubsystemRead}), "armed a subsystem read");
    m.onRequestSent(g_nowMs);
    check(m.onResponse(ack(0x3C, 0x0000'0042u)),
          "a subsystem read matches on the address alone");
    const auto reply = m.takeReply();
    check(reply->outcome == Outcome::Answered, "answered");
    check(reply->data == 0x0000'0042u, "and yields the value READ, not the echo");
    check(reply->addr == 0x3C, "reported against the address asked for");
}

static void testRefusal()
{
    Hl2ControlRequest m;
    check(m.arm({0x0E, 0x1234u, Echo::Exact}), "armed");
    m.onRequestSent(g_nowMs);
    // "Always send a response, may be error": the FSM substitutes 6'h3f for the
    // address when a subsystem was not ready. It is an ANSWER — the response
    // register has been consumed — so the slot frees immediately, unlike a
    // timeout.
    check(m.onResponse(ack(kRespAddrError, 0)), "a 0x3F reply matches whatever was asked");
    const auto reply = m.takeReply();
    check(reply->outcome == Outcome::Refused, "outcome is Refused");
    check(reply->addr == 0x0E,
          "the verdict names the address ASKED FOR, not the refusal encoding");
    check(m.state() == State::Idle, "a refusal frees the slot: the radio is done with it");
    check(m.refusals() == 1, "counted");
}

static void testTimeoutQuarantinesTheLateEcho()
{
    // This is the property §13's "no transaction id" warning is really about.
    constexpr int kDeadline = 8;
    constexpr int kQuarantine = 8;
    // Floor 0: this test is about the frame COUNT and the quarantine handover.
    // The floor has its own tests below.
    Hl2ControlRequest m(kDeadline, kQuarantine, 0);

    check(m.arm({0x0E, 0xAAAA'AAAAu, Echo::Exact}), "armed");
    m.onRequestSent(g_nowMs);
    runFrames(m, kDeadline - 1);
    check(m.state() == State::Awaiting, "still waiting one frame short of the deadline");
    m.onEp6Frame(g_nowMs);
    check(m.state() == State::Settled, "the deadline settles a verdict");
    check(m.timeouts() == 1, "counted");

    check(!m.arm({0x14, 1, Echo::Exact}), "no re-arm while the verdict is unread");
    const auto reply = m.takeReply();
    check(reply->outcome == Outcome::TimedOut, "outcome is TimedOut");
    check(reply->addr == 0x0E, "named");

    // NOT Idle. The radio never told us it was finished with that request, so
    // it may still answer it.
    check(m.state() == State::Quarantine, "a timeout quarantines rather than freeing the slot");
    check(!m.arm({0x14, 1, Echo::Exact}), "no new request may be armed during the quarantine");

    // The late echo arrives. On the wire it is byte-identical to a timely reply.
    check(!m.onResponse(ack(0x0E, 0xAAAA'AAAAu)),
          "the abandoned request's own echo is SWALLOWED, not re-paired");
    check(m.staleAcks() == 1, "and counted, which is how an operator would ever see this");
    check(m.state() == State::Quarantine, "swallowing it does not settle anything");
    check(m.takeReply() == std::nullopt, "and produces no second verdict");

    runFrames(m, kQuarantine - 1);
    check(m.state() == State::Quarantine, "the quarantine runs its full length");
    m.onEp6Frame(g_nowMs);
    check(m.state() == State::Idle, "then the slot reopens");
    check(m.arm({0x14, 1, Echo::Exact}), "and a new request is accepted");

    // And the point of all of it: had the quarantine not been there, THIS is
    // the pairing that would have happened — a request at the same register,
    // answered by the previous request's echo.
    m.onRequestSent(g_nowMs);
    check(!m.onResponse(ack(0x0E, 0xAAAA'AAAAu)),
          "the old echo cannot match the new request either");

    // THE RESIDUAL, pinned here so the header's claim stays honest. An earlier
    // version of that header said a late reply had nowhere to land "by
    // construction". Quarantine makes a wrong pairing UNLIKELY; it cannot make
    // it impossible. A caller that re-issues the IDENTICAL request after a
    // timeout is asking for a reply byte-for-byte equal to the one it
    // abandoned, and no matching rule can separate those two. That is a
    // property of a wire with no transaction id, not a defect in this class —
    // and it is most of why the wall-clock floor matters, since the floor is
    // what keeps the abandoned reply from still being in flight at all.
    Hl2ControlRequest again(kDeadline, kQuarantine, 0);
    check(again.arm({0x0E, 0xAAAA'AAAAu, Echo::Exact}), "armed");
    again.onRequestSent(g_nowMs);
    runFrames(again, kDeadline);
    (void)again.takeReply();
    runFrames(again, kQuarantine);
    check(again.state() == State::Idle, "the quarantine elapsed");
    check(again.arm({0x0E, 0xAAAA'AAAAu, Echo::Exact}), "the SAME request, re-issued");
    again.onRequestSent(g_nowMs);
    check(again.onResponse(ack(0x0E, 0xAAAA'AAAAu)),
          "an identical re-issue CANNOT be told from the abandoned reply — "
          "documented, not fixed, because the wire carries no id");
}

// ---- the wall-clock floor (blocker 1) --------------------------------------

static void testTheDeadlineHasAWallClockFloor()
{
    // 32 frames is 42 ms only at 48 kHz with ONE receiver. A frame carries
    // 504/(6*numRx+2) rounds, so frames per second rise with both the sample
    // rate and the receiver count; at 384 kHz with the three receivers
    // maxReceiversAtRate() admits, the same 32 frames is 2.08 ms and deadline
    // plus quarantine is 4.17 ms. docs/HERMES.md records a 6.08 ms worst-case
    // EP6 inter-arrival gap, which is HOST-SIDE and wall-clock: the frame count
    // cannot see it, and could expire twice over inside one delivery gap.
    check(ep6RoundsPerFrame(1) == 63, "48 kHz / 1 RX: 63 rounds per frame");
    check(ep6RoundsPerFrame(3) == 25, "384 kHz / 3 RX: 25 rounds per frame");
    check(maxReceiversAtRate(384000) == 3, "the link budget admits 3 RX at 384 kHz");

    g_nowMs = 0;
    Hl2ControlRequest m;                    // the SHIPPING constants, floor included
    check(m.arm({0x0E, 0x1234u, Echo::Exact}), "armed");
    m.onRequestSent(g_nowMs);

    runFramesAtRate(m, Hl2ControlRequest::kDefaultDeadlineFrames, 384000, 3);
    check(g_nowMs <= 3, "32 frames at 384 kHz with 3 receivers is ~2 ms of wall clock");
    check(m.state() == State::Awaiting,
          "the frame count ALONE does not expire the deadline any more");

    runFramesAtRate(m, 600, 384000, 3);     // ~39 ms, hundreds of slots
    check(g_nowMs < Hl2ControlRequest::kDefaultFloorMs, "still inside the floor");
    check(m.state() == State::Awaiting, "the wall-clock floor is what holds it open");

    // And the answer lands. Under the frame count alone this reply would have
    // been counted stale against a request already abandoned and quarantined.
    check(m.onResponse(ack(0x0E, 0x1234u)),
          "a reply inside the floor is the ANSWER, not a stale ACK");
    check(m.takeReply()->outcome == Outcome::Answered, "answered");
    check(m.staleAcks() == 0, "and nothing was counted stale");

    // The floor does expire, at the 48 kHz budget it is derived from.
    g_nowMs = 0;
    Hl2ControlRequest n;
    check(n.arm({0x0E, 1, Echo::Exact}), "armed");
    n.onRequestSent(g_nowMs);
    runFramesAtRate(n, 2000, 384000, 3);    // ~130 ms: both halves satisfied
    check(n.state() == State::Settled, "past the floor it times out as it always did");
    check(n.takeReply()->outcome == Outcome::TimedOut, "timed out");
}

static void testTheFrameCountStillRulesAStoppedStream()
{
    // The floor is a FLOOR, never a substitute. resp_rqst toggles only while
    // `run` is set, so a radio that stopped streaming owes no slots — and
    // however much wall-clock time passes, nothing times out. This is the
    // property the frame count was chosen for and it has to survive the fix.
    g_nowMs = 0;
    Hl2ControlRequest m;
    check(m.arm({0x0E, 1, Echo::Exact}), "armed");
    m.onRequestSent(g_nowMs);
    g_nowMs = 10'000;                       // ten seconds of silence
    check(m.state() == State::Awaiting,
          "no frames, no timeout — a wall clock alone would have blamed the radio");

    runFrames(m, Hl2ControlRequest::kDefaultDeadlineFrames);
    check(m.state() == State::Settled, "the frames are what expire it, once they come");
    check(m.takeReply()->outcome == Outcome::TimedOut, "timed out");
    check(m.state() == State::Quarantine, "quarantined");

    // The quarantine has its own floor, running from the moment we gave up —
    // which is what it has to outlast, not the moment the caller looked.
    runFrames(m, Hl2ControlRequest::kDefaultQuarantineFrames * 4);
    check(m.state() == State::Quarantine, "frames alone do not release the quarantine");
    g_nowMs += Hl2ControlRequest::kDefaultFloorMs;
    runFrames(m, 1);
    check(m.state() == State::Idle, "it releases once BOTH have passed");
}

static void testOneDeliveryGapNoLongerWalksTheWholeMachine()
{
    // Blocker 1's concrete consequence. At 384 kHz with 3 receivers the 6.08 ms
    // gap docs/HERMES.md records is ~93 frames, and they arrive in ONE
    // onReadyRead() drain — wall-clock nearly instantaneous. 93 frames is the
    // entire deadline and the entire quarantine with frames to spare.
    const double framesPerSecond = 384000.0 / ep6RoundsPerFrame(3);
    const int framesInTheGap = static_cast<int>(6.08e-3 * framesPerSecond);
    check(framesInTheGap > Hl2ControlRequest::kDefaultDeadlineFrames
                               + Hl2ControlRequest::kDefaultQuarantineFrames,
          "one recorded delivery gap is more frames than deadline plus quarantine");

    g_nowMs = 0;
    Hl2ControlRequest m;
    check(m.arm({0x0E, 0x00AB'CD12u, Echo::Exact}), "armed");
    m.onRequestSent(g_nowMs);
    g_nowMs = 7;                            // the gap itself, in wall clock
    runFrames(m, framesInTheGap);           // drained in a single pass
    check(m.state() == State::Awaiting,
          "a backlog of frames does not consume a request seven milliseconds old");
    check(m.onResponse(ack(0x0E, 0x00AB'CD12u)), "the radio's answer still lands");
    check(m.answered() == 1 && m.staleAcks() == 0,
          "as an answer, not as a stale ACK against an abandoned request");
}

static void testResetClearsEvenTheQuarantine()
{
    Hl2ControlRequest m(4, 1000, 0);
    check(m.arm({0x0E, 1, Echo::Exact}), "armed");
    m.onRequestSent(g_nowMs);
    runFrames(m, 4);
    (void)m.takeReply();
    check(m.state() == State::Quarantine, "quarantined");
    // A stream that has stopped and restarted cannot deliver a reply from
    // before it, so the quarantine has nothing left to protect against.
    m.reset();
    check(m.state() == State::Idle, "reset clears the quarantine");
    check(m.arm({0x0E, 1, Echo::Exact}), "and the slot is immediately usable");
}

static void testAckWithNothingOutstandingIsStale()
{
    Hl2ControlRequest m;
    check(!m.onResponse(ack(0x0E, 1)), "an ACK in Idle is refused");
    check(m.staleAcks() == 1,
          "an ACK nobody asked for is the signature of a second client on the radio");
    check(m.state() == State::Idle, "and changes nothing");
}

// The C0 encoding both directions, against the gateware's own composition.
static void testWireEncoding()
{
    // Radio->host ACK: C0 = {1'b1, resp_cmd_addr[5:0], ptt_resp}. A six-bit
    // address, so 0x3B (the AD9866 SPI register) is expressible — which the
    // classic four-bit C0[6:3] read is not.
    std::uint8_t frame[8] = {0x7F, 0x7F, 0x7F, 0, 0x12, 0x34, 0x56, 0x78};
    frame[3] = static_cast<std::uint8_t>(0x80 | (0x3B << 1) | 0x01);
    const auto parsed = parseEp6Response(frame);
    check(parsed.has_value(), "sync-framed");
    check(parsed->ack, "C0[7] set reads as ACK");
    check(parsed->raddr == 0x3B, "ACK raddr is the full six bits");
    check(parsed->ptt, "C0[0] is ptt_resp in an ACK too");
    check(parsed->data == 0x12345678u, "C1..C4 big-endian");

    // Host->radio: withRespRqst and withMox are orthogonal, and neither can set
    // the other's bit. That is what lets a request ride an unkeyed frame and a
    // keyed one alike without either flag leaking.
    const Cc base = ccRegister(0x0E, 0);
    check((base[0] & kC0MoxBit) == 0, "ccRegister never sets MOX");
    check((base[0] & kC0RespRqstBit) == 0, "ccRegister never sets RQST");
    check((withRespRqst(base, true)[0] & kC0MoxBit) == 0, "withRespRqst leaves MOX alone");
    check((withMox(base, true)[0] & kC0RespRqstBit) == 0, "withMox leaves RQST alone");
    check(withRespRqst(withMox(base, true), true)[0]
              == withMox(withRespRqst(base, true), true)[0],
          "the two compose in either order");
    // An address that would have spilled into the RQST bit is masked, not
    // wrapped into a keyed frame.
    check((ccRegister(0x7F, 0)[0] & kC0RespRqstBit) == 0,
          "an out-of-range address cannot alias into RQST");
    check((ccRegister(0x7F, 0)[0] & kC0MoxBit) == 0,
          "nor into MOX");
}

// An ACK is not telemetry. Before this guard, an ACK for register 0x00 would be
// decoded as a firmware version, an ADC-overload flag and a TX FIFO depth,
// invented out of the bytes we ourselves sent.
static void testAckDoesNotPoisonTelemetry()
{
    Hl2Telemetry t;
    t.apply(classic(0x00, 0x0000'0049u));       // real telemetry: firmware 0x49
    check(t.firmwareVersion.has_value() && *t.firmwareVersion == 0x49,
          "the free-running cycle still populates telemetry");

    // Our own echo of a config-register write, coming back as an ACK at
    // raddr 0x00. Every field it would have overwritten must be untouched.
    t.apply(ack(0x00, 0xFFFF'FFFFu));
    check(*t.firmwareVersion == 0x49, "an ACK does not rewrite the firmware version");
    check(!t.adcOverload.value_or(false), "nor invent an ADC overload");

    // PTT is the exception, and legitimately so: C0[0] is ptt_resp in both
    // branches of the gateware's iresp composition.
    Ep6Response keyed = ack(0x00, 0);
    keyed.ptt = true;
    t.apply(keyed);
    check(t.ptt, "an ACK's PTT bit is still honoured");
}

int main()
{
    testAddressSpace();
    testWireBank();
    testSingleOutstanding();
    testEchoMatchIsNarrow();
    testSubsystemReadMatchesOnAddressOnly();
    testRefusal();
    testTimeoutQuarantinesTheLateEcho();
    testTheDeadlineHasAWallClockFloor();
    testTheFrameCountStillRulesAStoppedStream();
    testOneDeliveryGapNoLongerWalksTheWholeMachine();
    testResetClearsEvenTheQuarantine();
    testAckWithNothingOutstandingIsStale();
    testWireEncoding();
    testAckDoesNotPoisonTelemetry();

    if (g_failures == 0)
        std::printf("hl2_rqst_ack_test: OK\n");
    return g_failures == 0 ? 0 : 1;
}
