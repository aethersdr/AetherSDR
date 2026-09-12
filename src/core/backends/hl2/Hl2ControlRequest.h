#pragma once

#include <cstdint>
#include <optional>

#include "core/backends/hl2/MetisProtocol.h"

// The Hermes-Lite 2 RQST/ACK state machine (docs/HERMES.md §13 item 13,
// oracle §5). Qt-free and socket-free, like MetisProtocol, so it unit-tests
// against synthetic responses with no radio and no event loop.
//
// ---------------------------------------------------------------------------
// THIS IS NOT AN RPC, AND MODELLING IT AS ONE IS THE FAILURE MODE
// ---------------------------------------------------------------------------
//
// There is no call, no future, no correlation id, and no guarantee of an
// answer. Three properties of the wire make an RPC abstraction actively
// dangerous here, and each one is answered by a specific piece of this class:
//
//  1. SINGLE OUTSTANDING. The gateware's response register holds exactly one
//     reply — control.v says so in a comment on the line that writes it
//     ("Queue size is 1"). Worse than dropping a second request, the response
//     FSM SILENTLY DISCARDS the first: a command arriving while the FSM sits in
//     RESP_ACK or RESP_READ gets no reply at all, and one arriving in RESP_WAIT
//     overwrites the saved address and data of the request still waiting for a
//     slot. So a pipelined pair does not give you two answers late; it gives
//     you one answer and one silence, and no error anywhere.
//
//     BOTH OF THOSE ARE GATED ON THE RQST BIT, which is what makes this a host
//     discipline problem rather than an impossibility. control.v enters the
//     sequence only on `cmd_rqst & cmd_requires_resp & ~cmd_is_alt`, and the
//     RESP_WAIT overwrite is inside `if (resp_rqst & ~resp_cnt)` and then
//     `if (cmd_rqst & cmd_requires_resp)` — `cmd_requires_resp` being C0[7],
//     wired from `cmd_resprqst` in hermeslite_core.v. The ordinary round robin,
//     which never sets C0[7], therefore CANNOT clobber a pending reply however
//     fast it runs. Only another RQST can, and only one party issues those.
//     --> arm() REFUSES unless the machine is Idle, and says so in its return
//         type. There is no queue. A caller that wants two reads does two.
//
//  2. ECHO-MATCHED, NO TRANSACTION ID. The reply carries the six-bit command
//     address in C0[6:1] and, for a plain register write, an echo of the data
//     we sent (control.v RESP_START: resp_cmd_data_next = cmd_data). That is
//     the whole of the correspondence. Matching is therefore a JUDGEMENT about
//     equality, not a lookup, and getting it wrong pairs a reply with the wrong
//     request in a way that looks exactly like success.
//     --> matches() is explicit and narrow: address equality always, plus data
//         equality for Echo::Exact requests. A response that fails it is not
//         "probably ours" — it is counted as stale and dropped.
//
//  3. A LATE REPLY IS INDISTINGUISHABLE FROM A TIMELY ONE. With no id, an
//     answer to a request we gave up on looks identical to an answer to the
//     request we just made — IF the two can ever be in flight together.
//     --> they cannot be, by construction. A deadline that expires does not
//         return the machine to Idle; it moves it to Quarantine, where every
//         ACK is swallowed and counted, and arm() stays refused until the
//         quarantine elapses. The abandoned reply has nowhere to land but
//         Quarantine. This is the piece an RPC layer would have optimised away.
//
// ---------------------------------------------------------------------------
// TWO MORE FACTS THE WIRE IMPOSES
// ---------------------------------------------------------------------------
//
// THE CLOCK IS EP6 FRAMES, NOT MILLISECONDS. resp_rqst toggles once per
// 512-byte EP6 frame (usopenhpsdr1.v, SYNC_RESP) and the whole EP6 emit path is
// gated on `run`. An idle radio answers discovery and nothing else: no stream,
// no response slots, no replies, for ever. A wall-clock deadline would report
// "the radio timed out" for a condition that is really "we never gave it an
// opportunity to answer" — and would expire faster at 384 kHz than at 48 kHz
// for no reason on the radio's side. So the deadline is counted in the radio's
// own response slots, fed by onEp6Frame().
//
// THERE IS NO READ-ONLY REQUEST. Bit 7 does not mean "read". The gateware
// applies the write regardless and then echoes it; the only commands whose
// reply is not an echo are the AD9866 SPI and I2C subsystem writes, which carry
// a read opcode in their data and come back with the read value instead
// (RESP_READ, Echo::SubsystemRead below). Every RQST this machine issues is a
// WRITE that asks to be acknowledged. Callers, and reviewers, have to read it
// that way.
//
namespace AetherSDR::hl2 {

class Hl2ControlRequest {
public:
    // How the reply's DATA relates to the request's data, which is the whole of
    // the matching judgement beyond the address.
    enum class Echo {
        // A plain register write. The reply's C1..C4 are a byte-for-byte echo of
        // what we sent, so data equality is available and is USED: it is the
        // only evidence, beyond a six-bit address, that this reply belongs to
        // this request rather than to the last one at the same register.
        Exact,
        // A command whose reply carries the value READ rather than the bytes
        // written: the AD9866 SPI command (0x3b) and both I2C buses (0x3c
        // internal, 0x3d the external companion bus — MetisProtocol.h documents
        // TWO, and picking Echo::Exact for either would silently throw away the
        // data half of the match). Of those, only 0x3b is reachable through
        // MetisClient::requestRegister's allow-list today.
        //
        // Only the address can be matched, so such a request is strictly weaker
        // evidence — which is exactly why the quarantine on timeout is not
        // optional.
        SubsystemRead,
    };

    enum class State {
        Idle,        // nothing outstanding; arm() is accepted
        Queued,      // armed, bank not yet on the wire; the deadline has not started
        Awaiting,    // bank sent, counting EP6 frames against the deadline
        Settled,     // a verdict is waiting for takeReply()
        Quarantine,  // deadline blown; swallowing whatever the radio still owes us
    };

    enum class Outcome {
        Answered,   // a reply matched. `data` is the echo, or the subsystem read value
        Refused,    // the radio answered with raddr 0x3F: a subsystem was not ready
        TimedOut,   // the deadline passed with no matching reply. See the class note
    };

    struct Request {
        int           addr = 0;
        std::uint32_t data = 0;
        Echo          echo = Echo::Exact;
    };

    struct Reply {
        Outcome       outcome = Outcome::TimedOut;
        int           addr = 0;     // the request's address, not the ACK's
        std::uint32_t data = 0;     // meaningless unless outcome == Answered
    };

    // Deadline and quarantine, in EP6 FRAMES. See the class note for why the
    // unit is frames.
    //
    // The gateware answers fast, but NOT every frame, and the difference is
    // worth stating because 32 was sized against it. control.v toggles
    // `resp_cnt` on every `resp_rqst` and then acts "Only every other
    // resp_rqst" (its own comment): both the RESP_WAIT exit and the write of
    // the command reply into `iresp` are guarded by `~resp_cnt`. So a COMMAND
    // RESPONSE SLOT OPENS ON ALTERNATE EP6 FRAMES, not on every frame — the
    // free-running telemetry slots are the other half of that alternation.
    //
    // RESP_START latches on the command and RESP_ACK/RESP_READ each take a
    // cycle or two of clk_ctrl, which is nothing beside a frame; the wait for a
    // slot is the whole of the latency, and it is one frame at best and two at
    // worst depending on the phase `resp_cnt` happens to be in. Call it two to
    // four frames end to end. 32 frames is therefore SIXTEEN response
    // opportunities, not thirty-two — still generous enough to absorb an EP2
    // pacer tick, the round trip and a slow I2C subsystem, and at 48 kHz with
    // one receiver (63 samples per 512-byte frame) it is ~42 ms, which is short
    // enough that a genuinely dead request does not strand a caller. The
    // constant is unchanged because the alternation was already counted; what
    // was missing was saying so here.
    static constexpr int kDefaultDeadlineFrames = 32;
    // Equal to the deadline: whatever the radio still owes us is released on the
    // next slot after it becomes available, so one more deadline's worth of
    // frames is past any reply that could still be in flight for the abandoned
    // request. Symmetry is the point — a quarantine shorter than the deadline
    // would leave a window where a late reply meets a new request.
    static constexpr int kDefaultQuarantineFrames = 32;

    Hl2ControlRequest() = default;
    Hl2ControlRequest(int deadlineFrames, int quarantineFrames) noexcept
        : m_deadlineFrames(deadlineFrames > 0 ? deadlineFrames : 1)
        , m_quarantineFrames(quarantineFrames > 0 ? quarantineFrames : 0)
    {}

    // Addresses this machine will carry. 0x00..0x3E: six bits, minus 0x3F,
    // which the radio uses to SAY "refused" and which is also the extended-
    // address escape. Requesting it would make a refusal and an answer the
    // same bytes.
    [[nodiscard]] static constexpr bool isRequestableAddress(int addr) noexcept
    {
        return addr >= 0 && addr < kRespAddrError;
    }

    // Take the one outstanding slot. Returns FALSE — and changes nothing — if
    // the machine is not Idle, or the address is not requestable.
    //
    // [[nodiscard]] on purpose. A refused request that is treated as sent is the
    // bug this whole class exists to prevent, and the compiler can catch it.
    [[nodiscard]] bool arm(const Request& r) noexcept;

    [[nodiscard]] State state() const noexcept { return m_state; }
    [[nodiscard]] const Request& outstanding() const noexcept { return m_request; }

    // The C&C bank to put on the wire, with the RQST bit set. Non-empty ONLY in
    // Queued, so a caller that polls it every EP2 frame sends the request
    // exactly once and never re-requests from the round robin.
    //
    // MOX IS NOT SET HERE and cannot be: ccRegister() shifts the address left,
    // leaving C0[0] clear, and withRespRqst() touches C0[7] only. Keying stays
    // the sole business of MetisClient's transmit gate.
    [[nodiscard]] std::optional<Cc> wireBank() const noexcept;

    // Call when the bank from wireBank() has actually been handed to the
    // socket. Queued -> Awaiting; this is when the deadline starts counting,
    // because a request still sitting behind a one-shot queue has not yet given
    // the radio anything to answer.
    void onRequestSent() noexcept;

    // Advance one EP6 FRAME — one response slot. Drives both the deadline and
    // the quarantine. Call it once per 512-byte frame, not once per packet.
    void onEp6Frame() noexcept;

    // Offer a decoded EP6 response. Returns true if it was consumed as this
    // request's reply.
    //
    // Non-ACK responses are not this machine's business and are rejected
    // without comment — they are the free-running telemetry cycle. An ACK that
    // does not match, or that arrives with nothing outstanding, is counted as
    // stale and dropped: see staleAcks().
    bool onResponse(const Ep6Response& r) noexcept;

    // The matching judgement, exposed because it is the part most worth
    // testing directly and most dangerous to get wrong.
    [[nodiscard]] bool matches(const Ep6Response& r) const noexcept;

    // The verdict, once. Settled -> Idle for an answer or a refusal; Settled ->
    // Quarantine for a timeout, because the radio may still answer the request
    // we just gave up on.
    [[nodiscard]] std::optional<Reply> takeReply() noexcept;

    // Forget everything, quarantine included. For a link that went down: the
    // radio's response register does not survive a metis-stop, and a stream
    // that has restarted cannot deliver a reply from before it.
    void reset() noexcept;

    // Counters. staleAcks() is the one to watch: it is non-zero exactly when
    // something answered that nothing had asked for, which on this protocol
    // means either a reply we abandoned or a second client on the same radio.
    [[nodiscard]] std::uint64_t answered() const noexcept { return m_answered; }
    [[nodiscard]] std::uint64_t refusals() const noexcept { return m_refusals; }
    [[nodiscard]] std::uint64_t timeouts() const noexcept { return m_timeouts; }
    [[nodiscard]] std::uint64_t staleAcks() const noexcept { return m_staleAcks; }

private:
    void settle(Outcome outcome, std::uint32_t data) noexcept;

    State   m_state = State::Idle;
    Request m_request{};
    Reply   m_reply{};
    int     m_framesLeft = 0;          // deadline in Awaiting, quarantine in Quarantine

    int m_deadlineFrames   = kDefaultDeadlineFrames;
    int m_quarantineFrames = kDefaultQuarantineFrames;

    std::uint64_t m_answered  = 0;
    std::uint64_t m_refusals  = 0;
    std::uint64_t m_timeouts  = 0;
    std::uint64_t m_staleAcks = 0;
};

}  // namespace AetherSDR::hl2
