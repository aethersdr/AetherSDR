#pragma once

#include <QLineEdit>
#include <QPushButton>
#include <QString>

namespace AetherSDR {

struct IpConfigPresentationState {
    QString sessionKey;
    bool canConfigure{false};
};

// Dim with a reason, never hide (#5262 M3a, #4896): a hidden control is not
// announced, so a screen-reader user cannot learn the radio lacks it. `reason`
// is required because a dimmed control without one is barely better.
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
