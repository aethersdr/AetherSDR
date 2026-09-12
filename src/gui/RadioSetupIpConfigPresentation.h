#pragma once

#include <QLineEdit>
#include <QPushButton>
#include <QString>

namespace AetherSDR {

struct IpConfigPresentationState {
    QString sessionKey;
    bool canConfigure{false};
};

// DIM WITH A REASON, not hide (#5262 M3a doctrine; #4896).
//
// This was a bare setVisible(), and #5299 applied it to twelve Radio Setup
// surfaces four days after #5266 had given the Enforce Private IP button a
// disabled state, a tooltip and an accessibleDescription — deleting all three.
// A hidden control tells a blind operator nothing at all: it is not announced,
// so there is no way to learn the radio simply does not support it.
//
// `reason` is required for the same regression not to recur: a dimmed control
// with no stated cause is only marginally better than a hidden one.
inline void applyCapabilitySurfaceAvailability(QWidget* surface,
                                               bool connected,
                                               bool supported,
                                               const QString& reason)
{
    // Permissive on disconnect, like every gate in applyCapabilitiesToUi.
    const bool available = !connected || supported;
    surface->setEnabled(available);
    surface->setToolTip(available ? QString() : reason);
    surface->setAccessibleDescription(available ? QString() : reason);
}

// Cohesive radio-specific CLUSTERS may still hide wholesale — that is the
// doctrine's one sanctioned hide, at applet/group granularity rather than per
// control. Kept distinct from the function above so a call site states which
// rule it is invoking.
inline void applyCapabilityClusterVisibility(QWidget* cluster,
                                             bool connected,
                                             bool supported)
{
    cluster->setVisible(!connected || supported);
}

inline void applyIpConfigPresentation(
    IpConfigPresentationState& state,
    const QString& sessionKey,
    bool canConfigure,
    bool isStatic,
    const QString& ip,
    const QString& netmask,
    const QString& gateway,
    QPushButton* dhcpButton,
    QPushButton* staticButton,
    QLineEdit* ipEdit,
    QLineEdit* maskEdit,
    QLineEdit* gatewayEdit,
    QPushButton* applyButton,
    const QString& unavailableTip)
{
    const bool sessionChanged = sessionKey != state.sessionKey
        || canConfigure != state.canConfigure;
    if (sessionChanged) {
        dhcpButton->setChecked(!isStatic);
        staticButton->setChecked(isStatic);
        ipEdit->setText(ip);
        maskEdit->setText(netmask);
        gatewayEdit->setText(gateway);
        applyButton->setEnabled(false);
        state = {sessionKey, canConfigure};
    }

    dhcpButton->setEnabled(canConfigure);
    staticButton->setEnabled(canConfigure);
    ipEdit->setEnabled(canConfigure && staticButton->isChecked());
    maskEdit->setEnabled(canConfigure && staticButton->isChecked());
    gatewayEdit->setEnabled(canConfigure && staticButton->isChecked());
    if (!canConfigure) {
        applyButton->setEnabled(false);
    }
    for (QWidget* control : {static_cast<QWidget*>(dhcpButton),
                             static_cast<QWidget*>(staticButton),
                             static_cast<QWidget*>(applyButton)}) {
        control->setToolTip(canConfigure ? QString() : unavailableTip);
        control->setAccessibleDescription(control->toolTip());
    }
}

} // namespace AetherSDR
