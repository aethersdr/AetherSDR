#include "core/backends/hl2/Hl2ControlRequest.h"

namespace AetherSDR::hl2 {

bool Hl2ControlRequest::arm(const Request& r) noexcept
{
    if (m_state != State::Idle)
        return false;                       // single outstanding; no queue
    if (!isRequestableAddress(r.addr))
        return false;
    m_request = r;
    m_reply = {};
    m_state = State::Queued;
    m_framesLeft = 0;                       // the deadline starts at onRequestSent()
    return true;
}

std::optional<Cc> Hl2ControlRequest::wireBank() const noexcept
{
    if (m_state != State::Queued)
        return std::nullopt;
    return withRespRqst(ccRegister(m_request.addr, m_request.data), true);
}

void Hl2ControlRequest::onRequestSent(std::int64_t nowMs) noexcept
{
    if (m_state != State::Queued)
        return;
    m_state = State::Awaiting;
    m_framesLeft = m_deadlineFrames;
    m_floorAtMs = nowMs + m_floorMs;
}

void Hl2ControlRequest::onEp6Frame(std::int64_t nowMs) noexcept
{
    switch (m_state) {
    case State::Awaiting:
        // AND, not OR, and the frames are decremented either way. The count can
        // run out long before the floor at a high sample rate — that is the
        // whole point — so it is clamped at zero rather than allowed to run
        // negative while the floor is still pending.
        if (m_framesLeft > 0)
            --m_framesLeft;
        if (m_framesLeft <= 0 && nowMs >= m_floorAtMs) {
            ++m_timeouts;
            // The quarantine's floor starts HERE, at the instant we gave up,
            // not at takeReply(): what it has to outlast is the reply still
            // owed for the request abandoned at this moment.
            m_floorAtMs = nowMs + m_floorMs;
            // Note the outcome is delivered, not swallowed: a caller must be
            // able to see that this radio did not answer. What it does NOT do is
            // free the slot — see takeReply().
            settle(Outcome::TimedOut, 0);
        }
        break;
    case State::Quarantine:
        if (m_framesLeft > 0)
            --m_framesLeft;
        if (m_framesLeft <= 0 && nowMs >= m_floorAtMs) {
            m_state = State::Idle;
            m_request = {};
        }
        break;
    case State::Idle:
    case State::Queued:
    case State::Settled:
        // Queued does NOT tick. A request waiting behind a one-shot queue has
        // not reached the radio, so frames that pass before it does are not
        // frames in which the radio failed to answer.
        break;
    }
}

bool Hl2ControlRequest::matches(const Ep6Response& r) const noexcept
{
    if (!r.ack)
        return false;                       // the free-running telemetry cycle
    if (m_state != State::Awaiting)
        return false;                       // nothing is outstanding to match
    // A refusal replaces the address with 0x3F, so it cannot be address-matched
    // against what we asked for — and it is unambiguous precisely because we
    // never request 0x3F (isRequestableAddress).
    if (r.raddr == kRespAddrError)
        return true;
    if (r.raddr != m_request.addr)
        return false;
    // Echo::SubsystemRead replies carry the value read, not the bytes written,
    // so the address is all the evidence there is. Echo::Exact replies carry
    // the echo, and we spend it: matching on the address alone would pair a
    // reply with any request at the same register, which after an abandoned
    // request is the wrong one.
    if (m_request.echo == Echo::Exact && r.data != m_request.data)
        return false;
    return true;
}

bool Hl2ControlRequest::onResponse(const Ep6Response& r) noexcept
{
    if (!r.ack)
        return false;

    if (!matches(r)) {
        // Every ACK that is not ours is stale by construction: nothing else on
        // this link sets the RQST bit, so an unmatched ACK is either the reply
        // to a request we abandoned (landing here in Quarantine, which is the
        // point of Quarantine) or evidence of a second client on the radio.
        ++m_staleAcks;
        return false;
    }

    if (r.raddr == kRespAddrError) {
        ++m_refusals;
        settle(Outcome::Refused, r.data);
    } else {
        ++m_answered;
        settle(Outcome::Answered, r.data);
    }
    return true;
}

std::optional<Hl2ControlRequest::Reply> Hl2ControlRequest::takeReply() noexcept
{
    if (m_state != State::Settled)
        return std::nullopt;
    const Reply out = m_reply;
    m_reply = {};
    if (out.outcome == Outcome::TimedOut && m_quarantineFrames > 0) {
        // The radio never said it was finished with this request, so it may
        // still answer it. Do not open the slot until any such answer has had
        // its chance to arrive and be swallowed.
        m_state = State::Quarantine;
        m_framesLeft = m_quarantineFrames;
    } else {
        // Answered or Refused: the radio's response register has been consumed
        // and its FSM is back at RESP_START. Nothing is owed.
        m_state = State::Idle;
        m_request = {};
    }
    return out;
}

void Hl2ControlRequest::reset() noexcept
{
    m_state = State::Idle;
    m_request = {};
    m_reply = {};
    m_framesLeft = 0;
    m_floorAtMs = 0;
}

void Hl2ControlRequest::settle(Outcome outcome, std::uint32_t data) noexcept
{
    m_reply.outcome = outcome;
    m_reply.addr = m_request.addr;
    m_reply.data = data;
    m_state = State::Settled;
    m_framesLeft = 0;
}

}  // namespace AetherSDR::hl2
