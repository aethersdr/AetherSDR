#pragma once

#include <cstdint>

namespace AetherSDR {

// Tracks the proof behind TCI's bounded Icom unkey presentation barrier.
// Local optimistic state never enters this object: only an accepted CI-V
// PTT readback may confirm or revoke confirmation of the active generation.
class IcomTciUnkeySettle {
public:
    enum class Confirmation {
        Ignored,
        PendingExpiry,
        Late,
    };

    enum class Expiry {
        Stale,
        Confirmed,
        TimedOut,
    };

    std::uint64_t begin() noexcept
    {
        m_activeGeneration = ++m_generation;
        m_awaitingGeneration = m_activeGeneration;
        m_confirmedGeneration = 0;
        return m_activeGeneration;
    }

    Confirmation confirm(bool transmitting) noexcept
    {
        if (m_awaitingGeneration == 0) {
            return Confirmation::Ignored;
        }
        if (transmitting) {
            // The latest accepted radio state is authoritative. A keyed
            // readback revokes any earlier off confirmation but keeps waiting
            // for a later off readback, even after the settle timer expires.
            m_confirmedGeneration = 0;
            return Confirmation::PendingExpiry;
        }
        if (m_activeGeneration == m_awaitingGeneration) {
            m_confirmedGeneration = m_awaitingGeneration;
            return Confirmation::PendingExpiry;
        }
        m_awaitingGeneration = 0;
        return Confirmation::Late;
    }

    Expiry expire(std::uint64_t generation) noexcept
    {
        if (generation == 0 || generation != m_activeGeneration) {
            return Expiry::Stale;
        }
        m_activeGeneration = 0;
        if (m_confirmedGeneration == generation) {
            m_awaitingGeneration = 0;
            m_confirmedGeneration = 0;
            return Expiry::Confirmed;
        }
        return Expiry::TimedOut;
    }

    void cancel() noexcept
    {
        ++m_generation;
        m_activeGeneration = 0;
        m_awaitingGeneration = 0;
        m_confirmedGeneration = 0;
    }

    [[nodiscard]] bool isSettling() const noexcept
    {
        return m_activeGeneration != 0;
    }

    [[nodiscard]] bool isAwaitingConfirmation() const noexcept
    {
        return m_awaitingGeneration != 0;
    }

    [[nodiscard]] std::uint64_t activeGeneration() const noexcept
    {
        return m_activeGeneration;
    }

private:
    std::uint64_t m_generation = 0;
    std::uint64_t m_activeGeneration = 0;
    std::uint64_t m_awaitingGeneration = 0;
    std::uint64_t m_confirmedGeneration = 0;
};

} // namespace AetherSDR
