#pragma once

#include <QtGlobal>

namespace AetherSDR {

// Transport liveness of one TCI socket, as a pure state machine: the caller
// supplies the time and performs the action. Any message from the peer (text,
// binary or pong) is evidence it is alive. After kIdleMs of silence one ping
// goes out; a peer silent for kSilentDropWindowMs is aborted. It never keys.
class TciKeepAlive {
public:
    static constexpr qint64 kIdleMs = 5000;
    static constexpr qint64 kSilentDropWindowMs = 15000;
    // A probe sent late, after a stall on our side, still gets this long.
    static constexpr qint64 kMinimumPongWaitMs = 5000;
    static constexpr qint64 kPollMs = 1000;

    enum class Action { None, Ping, Abort };

    explicit TciKeepAlive(qint64 nowMs = 0) : m_lastHeardMs(nowMs) {}

    void heard(qint64 nowMs)
    {
        if (m_expired) { return; }
        m_lastHeardMs = nowMs;
        m_probing = false;
    }

    // Abort is returned once; afterwards the state stays expired.
    Action poll(qint64 nowMs)
    {
        if (m_expired) { return Action::None; }
        const qint64 silent = nowMs - m_lastHeardMs;
        if (m_probing) {
            if (silent >= kSilentDropWindowMs && nowMs - m_probeSentMs >= kMinimumPongWaitMs) {
                m_expired = true;
                return Action::Abort;
            }
            return Action::None;
        }
        if (silent >= kIdleMs) {
            m_probing = true;
            m_probeSentMs = nowMs;
            return Action::Ping;
        }
        return Action::None;
    }

    qint64 silentMs(qint64 nowMs) const { return nowMs - m_lastHeardMs; }
    bool probing() const { return m_probing; }
    bool expired() const { return m_expired; }

private:
    qint64 m_lastHeardMs{0};
    qint64 m_probeSentMs{0};
    bool m_probing{false};
    bool m_expired{false};
};

} // namespace AetherSDR
