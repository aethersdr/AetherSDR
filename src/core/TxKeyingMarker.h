#pragma once

#include <QWidget>

namespace AetherSDR {

// Authoritative marker for controls that *key the transmitter* when activated —
// MOX/PTT, TUNE, ATU tune, CWX CW send, AX.25 packet send, and any future
// keying control. The #3646 automation bridge refuses invoke() on any widget
// carrying this marker unless AETHER_AUTOMATION_ALLOW_TX is set, so an agent can
// never key a live radio by accident during hardware-in-the-loop tests.
//
// This is a *positive* signal set at the keying control's creation site, which
// is far more robust than matching control names: a TX-capable control is
// guarded because it was explicitly declared keying, not because its label
// happened to contain a magic word. Name matching remains in the bridge only as
// a logged belt-and-suspenders fallback for controls that predate or forget the
// marker.
//
// Usage at the call site, right after creating the button:
//     m_moxBtn = new QPushButton("MOX");
//     markTxKeying(m_moxBtn);
inline constexpr char kTxKeyingProperty[] = "aetherTxKeying";

inline void markTxKeying(QWidget* w)
{
    if (w)
        w->setProperty(kTxKeyingProperty, true);
}

} // namespace AetherSDR
