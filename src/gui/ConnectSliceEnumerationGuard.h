#pragma once

#include <QtGlobal>

namespace AetherSDR {

// Time window in which the first slice after connect is treated as connect
// enumeration (sync-only), not a mid-session fallback. The radio enumerates
// slices in creation order and does not persist the active slice, so asserting
// active=1 on the first one would overwrite the radio's live choice; FlexLib
// likewise updates without sending (Slice.cs _UpdateActive(..., update_radio:
// false)). Armed before 'client gui', disarmed on the 'slice list' reply, 3 s
// backstop. Pure policy: caller injects nowMs. Tests:
// band_recall_slice_selection_policy_test, radiomodel_slice_connect_enumeration_test.
class ConnectSliceEnumerationGuard {
public:
    explicit ConnectSliceEnumerationGuard(int windowMs = 3000)
        : m_windowMs(windowMs)
    {
    }

    // Opens the connect enumeration window.
    void arm(qint64 nowMs)
    {
        m_untilMs = nowMs + m_windowMs;
        m_consulted = false;
    }

    // Closes the window immediately (e.g. on disconnect or slice list reply).
    void cancelArm()
    {
        m_untilMs = 0;
    }

    // True while the connect enumeration window is open.
    // Marks the guard as consulted if active.
    bool isActive(qint64 nowMs) const
    {
        if (nowMs >= 0 && nowMs < m_untilMs) {
            m_consulted = true;
            return true;
        }
        return false;
    }

    // True once armed and then expired without ever being consulted while open.
    bool expiredUnused(qint64 nowMs) const
    {
        return m_untilMs > 0 && nowMs >= m_untilMs && !m_consulted;
    }

    int windowMs() const { return m_windowMs; }

private:
    int          m_windowMs{3000};
    qint64       m_untilMs{0};
    mutable bool m_consulted{false};
};

}  // namespace AetherSDR
