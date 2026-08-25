#pragma once

#include <algorithm>
#include <cstdint>

namespace AetherSDR {

// Owns the time window during which the first slice to arrive after connecting
// to a radio is treated as initial connect enumeration (synchronization-only)
// rather than a mid-session fallback.
//
// Pure policy — no timers, no widgets, no clock. The caller injects nowMs and
// owns every side effect. Unit-tested in band_recall_slice_selection_policy_test
// and radiomodel_slice_connect_enumeration_test.
class ConnectSliceEnumerationGuard {
public:
    explicit ConnectSliceEnumerationGuard(int windowMs = 3000)
        : m_windowMs(windowMs)
    {
    }

    // Opens the connect enumeration window.
    void arm(int64_t nowMs)
    {
        m_untilMs = nowMs + m_windowMs;
    }

    // Closes the window immediately (e.g. on disconnect).
    void cancelArm()
    {
        m_untilMs = 0;
    }

    // True while the connect enumeration window is open.
    bool isActive(int64_t nowMs) const
    {
        return nowMs >= 0 && nowMs < m_untilMs;
    }

    int windowMs() const { return m_windowMs; }

private:
    int     m_windowMs{3000};
    int64_t m_untilMs{0};
};

}  // namespace AetherSDR
