#pragma once

#include <QtGlobal>

namespace AetherSDR {

// Owns the time window during which the first slice to arrive after connecting
// to a radio is treated as initial connect enumeration (synchronization-only)
// rather than a mid-session fallback.
//
// Background (Principle II: Radio-Authoritative Live State):
// When connecting to a FlexRadio with existing slices, the radio enumerates
// slices in slice-creation order, which is not necessarily the slice that is
// active on the radio. Furthermore, FlexRadio firmware does not persist the
// operator's active-slice selection across restarts. Asserting active=1 on the
// first enumerated slice would overwrite the radio's live active slice before
// subsequent slices even exist client-side. FlexLib avoids this by updating its
// internal state without transmitting an active command back to the radio
// (see FlexLib Slice.cs:2035: _UpdateActive(..., update_radio: false)).
//
// The window is armed by RadioModel right before 'client gui' registration
// is dispatched, and disarmed when the 'slice list' response is received. The
// timeout (default 3000ms) acts as a safety backstop.
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
