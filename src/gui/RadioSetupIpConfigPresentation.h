#pragma once

#include <QLineEdit>
#include <QPushButton>
#include <QString>

namespace AetherSDR {

struct IpConfigPresentationState {
    QString sessionKey;
    bool canConfigure{false};
};

inline void applyCapabilitySurfaceVisibility(QWidget* surface,
                                             bool connected,
                                             bool supported)
{
    surface->setVisible(!connected || supported);
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
