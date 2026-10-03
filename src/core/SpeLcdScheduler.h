#pragma once

namespace AetherSDR {
namespace Spe {

// I/O-free scheduler deciding WHEN a 0x80 display request may be sent;
// SpeConnection owns the timer and transport. Invariant: at most one request
// in flight and one timer armed; every trigger (idle cadence, keystroke ACK,
// corrupt-frame retry, lost-reply fallback) returns its full Effect as a value,
// tested in spe_protocol_test. Exception: a late reply after the lost-reply
// fallback is credited to the retry (no request id; see kLcdLostReplyMs).
// arm == Timer::None leaves the armed timer running; the owner stops it via
// reset().
class LcdScheduler {
public:
    enum class Timer { None, IdleGap, LostReply, RejectRetry };

    struct Effect {
        bool  sendRequest{false};
        Timer arm{Timer::None};
    };

    // Polling begins (floating presentation open on a live connection):
    // request the first frame without waiting a full gap.
    Effect enable()
    {
        m_enabled = true;
        m_outstanding = false;
        m_refreshPending = false;
        return send();
    }

    // Polling ends, or the transport dropped. The owner stops its timer.
    void reset()
    {
        m_enabled = false;
        m_outstanding = false;
        m_refreshPending = false;
    }

    // A checksum-valid display frame arrived. Pace the next request from
    // this reply — or immediately service a refresh a keystroke ACK asked
    // for while this request was still in flight.
    Effect replyValid()
    {
        if (!m_enabled) {
            return {};
        }
        m_outstanding = false;
        if (m_refreshPending) {
            return send();
        }
        return {false, Timer::IdleGap};
    }

    // A complete display frame failed validation in both its raw and
    // telnet readings (mid-transmit RF is the field case). The retry pause
    // supersedes the lost-reply fallback — the request IS resolved, just
    // uselessly — so a late-arriving corrupted frame cannot leave two
    // timers racing toward two sends.
    Effect replyRejected()
    {
        if (!m_enabled) {
            return {};
        }
        if (!m_outstanding) {
            // Nothing was asked for, so nothing is owed: a duplicate or
            // stray display-shaped frame arriving during an idle gap must
            // not collapse that gap to the 80 ms retry pause and pull the
            // next request forward. (A corrupted frame belonging to a
            // request the lost-reply fallback already replaced still
            // passes this guard — see the class comment; the protocol
            // gives nothing to correlate on.)
            return {};
        }
        m_outstanding = false;
        return {false, Timer::RejectRetry};
    }

    // The single armed timer fired, whichever role it held: an idle gap
    // (send the next request), a retry pause (send it now), or the
    // lost-reply fallback (the request is classified lost; retry).
    Effect timerFired()
    {
        if (!m_enabled) {
            return {};
        }
        m_outstanding = false;
        return send();
    }

    // A keystroke was ACKed: the amp's screen just changed, refresh it —
    // immediately when the line is free, otherwise as pending work the
    // moment the in-flight request resolves. Never as a second in-flight
    // request.
    Effect ackSeen()
    {
        if (!m_enabled) {
            return {};
        }
        if (m_outstanding) {
            m_refreshPending = true;
            return {};
        }
        return send();
    }

    bool requestOutstanding() const { return m_outstanding; }

private:
    Effect send()
    {
        m_outstanding = true;
        m_refreshPending = false;
        return {true, Timer::LostReply};
    }

    bool m_enabled{false};
    bool m_outstanding{false};
    bool m_refreshPending{false};
};

}  // namespace Spe
}  // namespace AetherSDR
