#pragma once

namespace AetherSDR {
namespace Spe {

// Deterministic, I/O-free scheduler for the LCD mirror's request pacing.
// SpeConnection owns the QTimer and the transport; this class owns the one
// decision PR #5542's review rounds found scattered across independent
// timers: WHEN a 0x80 display request may be sent. Its invariant is that
// requests are single-file — at most one in flight, and at most one timer
// armed — so every trigger (idle cadence, keystroke ACK, corrupted-frame
// retry, lost-reply fallback) flows through the same gate, and the
// no-overlap property is provable in spe_protocol_test without a fake
// amplifier: each event returns the complete effect (send or not, which
// timer to arm) as a value.
//
// ONE documented exception, and it cannot be closed here. If the
// lost-reply fallback fires on a request that was merely still arriving,
// this class credits that late reply to the retry that replaced it, and
// two requests stay on the wire until the next reset(). The protocol
// carries no request id, so no amount of state in this class can tell
// the two replies apart — see kLcdLostReplyMs in SpeConnection.h, whose
// width is what makes the case implausible. Treat "at most one in
// flight" as holding up to that misclassification, not past it.
//
// An Effect with arm == Timer::None leaves the currently armed timer
// running — it never means "stop"; the owner stops its timer when it
// calls reset() (polling disabled, or transport down).
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
