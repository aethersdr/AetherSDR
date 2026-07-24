#pragma once

#include <cstdint>

namespace AetherSDR {

// Tracks a single automation-originated transmit lease. Merely allowing TX
// automation does not arm the lease: an accepted bridge action that can key
// must call arm(). This distinction prevents the automation watchdog from
// claiming and terminating operator-, DAX-, or TCI-originated transmissions.
class AutomationTxWatchdog
{
public:
    enum class Decision : uint8_t {
        None,
        ForceUnkey,
    };

    void arm(int64_t nowMs)
    {
        m_armedAtMs = nowMs;
        m_keyedSinceMs = kUnset;
    }

    void cancel()
    {
        m_armedAtMs = kUnset;
        m_keyedSinceMs = kUnset;
    }

    bool isArmed() const { return m_armedAtMs != kUnset; }
    bool hasObservedKeyed() const { return m_keyedSinceMs != kUnset; }

    Decision poll(bool keyed, int64_t nowMs, int64_t maxKeyMs,
                  int64_t pendingLeaseMs)
    {
        if (!isArmed()) {
            return Decision::None;
        }

        if (!keyed) {
            if (hasObservedKeyed()
                || nowMs - m_armedAtMs > pendingLeaseMs) {
                cancel();
            }
            return Decision::None;
        }

        if (!hasObservedKeyed()) {
            m_keyedSinceMs = nowMs;
            return Decision::None;
        }

        if (nowMs - m_keyedSinceMs > maxKeyMs) {
            cancel();
            return Decision::ForceUnkey;
        }
        return Decision::None;
    }

private:
    static constexpr int64_t kUnset = -1;
    int64_t m_armedAtMs{kUnset};
    int64_t m_keyedSinceMs{kUnset};
};

} // namespace AetherSDR
