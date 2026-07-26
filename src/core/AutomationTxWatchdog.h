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

    // `alreadyKeyed` is the transmitter state sampled *before* the bridge action
    // that is arming this lease was issued. When it is true the lease refuses to
    // adopt the transmission that is already up: it cannot have been caused by
    // this action, and claiming it would force-unkey an operator-, DAX-, TCI-,
    // or beacon-originated transmission at maxKeyMs. Such a lease only becomes
    // adoptable after it observes the transmitter unkey, i.e. a real rising edge
    // it can attribute; failing that it expires with the pending window.
    void arm(int64_t nowMs, bool alreadyKeyed = false)
    {
        m_armedAtMs = nowMs;
        m_keyedSinceMs = kUnset;
        m_awaitingUnkey = alreadyKeyed;
    }

    void cancel()
    {
        m_armedAtMs = kUnset;
        m_keyedSinceMs = kUnset;
        m_awaitingUnkey = false;
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
            // The pre-existing transmission ended, so anything keyed from here
            // is a rising edge this lease can legitimately attribute to itself.
            m_awaitingUnkey = false;
            if (hasObservedKeyed()
                || nowMs - m_armedAtMs > pendingLeaseMs) {
                cancel();
            }
            return Decision::None;
        }

        if (m_awaitingUnkey) {
            // Someone else's transmission is still up. Never adopt it; let the
            // pending window retire this lease instead.
            if (nowMs - m_armedAtMs > pendingLeaseMs) {
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
    // True while a transmission that predates this lease is still keyed.
    bool    m_awaitingUnkey{false};
};

} // namespace AetherSDR
