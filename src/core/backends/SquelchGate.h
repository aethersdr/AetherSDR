#pragma once

#include <QMetaType>

#include <algorithm>
#include <optional>

namespace AetherSDR {

// Whether a receiver's squelch gate is passing audio (SliceDelta::squelchGate).
// Unavailable: no gate is running that the backend can read (squelch off,
// level 0, a mode without a readable gate, or a muted receiver).
enum class SquelchGateState { Unavailable, Closed, Open };

// Turns a per-block gate reading into the states worth publishing. A change to
// Open or Closed is held until `holdSeconds` of processed audio have passed
// since the last publication, so a gate chattering on its threshold publishes
// at most 1/holdSeconds times a second and still ends on the state it settles
// in. Unavailable is published at once. Counts processed samples, never the
// wall clock.
class SquelchGateReporter {
public:
    static constexpr double kDefaultHoldSeconds = 0.1;

    explicit SquelchGateReporter(double holdSeconds = kDefaultHoldSeconds) noexcept
        : m_holdSeconds(std::max(0.0, holdSeconds)) {}

    // One call per processed block: `gateOpen` is the gate's reading (nullopt:
    // none), `blockSeconds` that block's duration. Returns the state to publish.
    [[nodiscard]] std::optional<SquelchGateState> feed(std::optional<bool> gateOpen,
                                                       double blockSeconds) noexcept
    {
        const SquelchGateState now = !gateOpen ? SquelchGateState::Unavailable
                                    : (*gateOpen ? SquelchGateState::Open
                                                 : SquelchGateState::Closed);
        if (blockSeconds > 0.0) {
            m_sincePublishSeconds = std::min(m_sincePublishSeconds + blockSeconds, 1.0e9);
        }
        if (now == m_published) {
            return std::nullopt;
        }
        if (now != SquelchGateState::Unavailable && m_sincePublishSeconds < m_holdSeconds) {
            return std::nullopt;
        }
        m_published = now;
        m_sincePublishSeconds = 0.0;
        return now;
    }

    // Forget what was published: the next reading other than Unavailable is
    // published at once. For a new channel or a reconnected receiver.
    void reset() noexcept
    {
        m_published = SquelchGateState::Unavailable;
        m_sincePublishSeconds = 1.0e9;
    }

    [[nodiscard]] SquelchGateState published() const noexcept { return m_published; }

private:
    double m_holdSeconds;
    SquelchGateState m_published = SquelchGateState::Unavailable;
    double m_sincePublishSeconds = 1.0e9;
};

}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::SquelchGateState)
