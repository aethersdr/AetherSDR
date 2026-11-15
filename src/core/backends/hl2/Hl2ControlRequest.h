#pragma once

#include <cstdint>
#include <optional>

#include "core/backends/hl2/MetisProtocol.h"

// The Hermes-Lite 2 RQST/ACK state machine (docs/HERMES.md §13 item 13), Qt-
// and socket-free. Not an RPC: no correlation id, no guaranteed answer.
//  1. Single outstanding: the response register holds one reply (control.v,
//     "Queue size is 1"), so arm() refuses unless Idle. No queue.
//  2. No transaction id: the reply echoes the address in C0[6:1] (and our data
//     for a plain write); matches() checks both, other ACKs count as stale.
//  3. A late reply looks timely: an expired deadline enters Quarantine, which
//     swallows ACKs and refuses arm() until it elapses.
// The deadline is EP6 frames AND a wall-clock floor; both must elapse. No
// stream means no timeout, but 32 frames is 42 ms at 48 kHz/1 RX and ~2 ms at
// 384 kHz/3 RX (504/(6*numRx+2) rounds per frame). The clock is passed in.
// C0[7] means "acknowledge", not "read": every RQST is a write the gateware
// applies and echoes; only I2C subsystem writes return a read value.
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
        // The reply carries the value READ, not the bytes written: only the I2C
        // buses 0x3c (Versa clock, AD9866) and 0x3d (external bus). Not 0x3b:
        // control.v RESP_READ sources only cmd_resp_data_i2c ("FIXME: suppor
        // read cmd_resp_data_ad9866") and ad9866ctrl.v ties sdo low, so a 0x3b
        // ACK is the echo or the 0x3F refusal. Only the address can match, so
        // MetisClient::requestRegister refuses this mode today.
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

    // Deadline and quarantine, in EP6 frames. control.v acts on a command reply
    // only on alternate resp_rqst (`~resp_cnt`), so a response slot opens every
    // other frame; a reply takes 2-4 frames and 32 frames = 16 slots. Elapsed
    // time is the floor's job, not this count's.
    static constexpr int kDefaultDeadlineFrames = 32;
    // Equal to the deadline: a shorter quarantine would leave a window where a
    // late reply to the abandoned request meets a new one.
    static constexpr int kDefaultQuarantineFrames = 32;

    // Wall-clock floor (ms) that must elapse alongside the frame count, never
    // instead of it (a stopped stream still times nothing out). Derived: 32
    // frames at 48 kHz / 1 RX (63 rounds per frame) = 42 ms, so ~42 ms holds at
    // every rate; ~7x the 6.08 ms worst-case EP6 gap in docs/HERMES.md.
    static constexpr int kFloorReferenceRateHz = 48000;
    static constexpr int kDefaultFloorMs =
        kDefaultDeadlineFrames * ep6RoundsPerFrame(1) * 1000 / kFloorReferenceRateHz;
    static_assert(kDefaultFloorMs == 42, "the floor is the 48 kHz / 1 RX frame budget");

    Hl2ControlRequest() = default;
    // floorMs is explicit and has no default ON PURPOSE. Omitting the clock is
    // exactly the bug this ctor exists to stop being reachable by accident; a
    // test that wants to isolate the frame counting passes 0 and says so.
    Hl2ControlRequest(int deadlineFrames, int quarantineFrames, int floorMs) noexcept
        : m_deadlineFrames(deadlineFrames > 0 ? deadlineFrames : 1)
        , m_quarantineFrames(quarantineFrames > 0 ? quarantineFrames : 0)
        , m_floorMs(floorMs > 0 ? floorMs : 0)
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
    //
    // `nowMs` is a MONOTONIC millisecond count from any origin — it is only ever
    // differenced. It is passed in rather than read here so that this class has
    // no clock of its own: see the class note.
    void onRequestSent(std::int64_t nowMs) noexcept;

    // Advance one EP6 FRAME — one response slot. Drives both the deadline and
    // the quarantine. Call it once per 512-byte frame, not once per packet.
    //
    // A deadline expires only when the frame count has run out AND the
    // wall-clock floor has passed. Both, never either: the frame count is what
    // keeps a stopped stream from timing anything out, and the floor is what
    // keeps a fast stream from timing it out before the answer could physically
    // have been delivered. `nowMs` must come from the same clock as the value
    // handed to onRequestSent().
    void onEp6Frame(std::int64_t nowMs) noexcept;

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
    // The instant the frame count is ALLOWED to expire, on the caller's clock.
    // Set at onRequestSent() for the deadline and re-set the moment a deadline
    // blows for the quarantine that follows it — the origin is when we gave up,
    // which is what the quarantine has to outlast.
    std::int64_t m_floorAtMs = 0;

    int m_deadlineFrames   = kDefaultDeadlineFrames;
    int m_quarantineFrames = kDefaultQuarantineFrames;
    int m_floorMs          = kDefaultFloorMs;

    std::uint64_t m_answered  = 0;
    std::uint64_t m_refusals  = 0;
    std::uint64_t m_timeouts  = 0;
    std::uint64_t m_staleAcks = 0;
};

}  // namespace AetherSDR::hl2
