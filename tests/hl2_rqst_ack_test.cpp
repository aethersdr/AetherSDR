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
// Pure: no Qt, no socket, no radio, no clock. The only "time" is EP6 frames,
// which is the only clock the radio answers on anyway.

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

static void runFrames(Hl2ControlRequest& m, int n)
{
    for (int i = 0; i < n; ++i)
        m.onEp6Frame();
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

    m.onRequestSent();
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

    m.onRequestSent();
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
    m.onRequestSent();

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
    // 0x3c is the I2C command register; its reply carries the value READ, not
    // the bytes written (control.v RESP_READ overwrites resp_cmd_data). Demanding
    // an echo here would reject every successful read.
    Hl2ControlRequest m;
    check(m.arm({0x3C, 0x07EA0000u, Echo::SubsystemRead}), "armed a subsystem read");
    m.onRequestSent();
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
    m.onRequestSent();
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
    Hl2ControlRequest m(kDeadline, kQuarantine);

    check(m.arm({0x0E, 0xAAAA'AAAAu, Echo::Exact}), "armed");
    m.onRequestSent();
    runFrames(m, kDeadline - 1);
    check(m.state() == State::Awaiting, "still waiting one frame short of the deadline");
    m.onEp6Frame();
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
    m.onEp6Frame();
    check(m.state() == State::Idle, "then the slot reopens");
    check(m.arm({0x14, 1, Echo::Exact}), "and a new request is accepted");

    // And the point of all of it: had the quarantine not been there, THIS is
    // the pairing that would have happened — a request at the same register,
    // answered by the previous request's echo.
    m.onRequestSent();
    check(!m.onResponse(ack(0x0E, 0xAAAA'AAAAu)),
          "the old echo cannot match the new request either");
}

static void testResetClearsEvenTheQuarantine()
{
    Hl2ControlRequest m(4, 1000);
    check(m.arm({0x0E, 1, Echo::Exact}), "armed");
    m.onRequestSent();
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
    testResetClearsEvenTheQuarantine();
    testAckWithNothingOutstandingIsStale();
    testWireEncoding();
    testAckDoesNotPoisonTelemetry();

    if (g_failures == 0)
        std::printf("hl2_rqst_ack_test: OK\n");
    return g_failures == 0 ? 0 : 1;
}
