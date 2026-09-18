#pragma once

#include <cstdint>

namespace AetherSDR::icom {

// Bounded lifecycle for the IC-705's one-shot NTP access command. The radio's
// 1A 08 result can remain 00 forever when the initiating command or terminal
// reply is lost, so polling must have an engine-owned terminal deadline.
class IcomNtpAccess {
public:
    static constexpr std::int64_t kTimeoutMs = 30'000;

    void start(std::int64_t nowMs) noexcept
    {
        m_state = State::Active;
        m_deadlineMs = nowMs + kTimeoutMs;
    }

    void finish() noexcept
    {
        m_state = State::Idle;
        m_deadlineMs = 0;
    }

    [[nodiscard]] bool expire(std::int64_t nowMs) noexcept
    {
        if (m_state != State::Active || nowMs < m_deadlineMs) {
            return false;
        }
        m_state = State::TimedOut;
        m_deadlineMs = 0;
        return true;
    }

    [[nodiscard]] bool active() const noexcept { return m_state == State::Active; }
    [[nodiscard]] bool timedOut() const noexcept { return m_state == State::TimedOut; }

private:
    enum class State { Idle, Active, TimedOut };
    State m_state = State::Idle;
    std::int64_t m_deadlineMs = 0;
};

}  // namespace AetherSDR::icom
