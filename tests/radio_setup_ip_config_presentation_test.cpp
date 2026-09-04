#include "gui/RadioSetupIpConfigPresentation.h"

#include <QApplication>
#include <iostream>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    bool ok = true;
    const auto check = [&ok](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ok = false;
        }
    };

    QPushButton dhcp("DHCP"), staticButton("Static"), apply("Apply");
    dhcp.setCheckable(true);
    staticButton.setCheckable(true);
    QLineEdit ip, mask, gateway;
    IpConfigPresentationState state;
    const QString unavailable = QStringLiteral("unsupported");

    QWidget gatedSurface;
    applyCapabilitySurfaceVisibility(&gatedSurface, true, false);
    check(gatedSurface.isHidden(),
          "unsupported capability surface is hidden while connected");
    applyCapabilitySurfaceVisibility(&gatedSurface, false, false);
    check(!gatedSurface.isHidden(),
          "unsupported capability surface becomes permissive on disconnect");
    applyCapabilitySurfaceVisibility(&gatedSurface, true, true);
    check(!gatedSurface.isHidden(),
          "supported capability surface remains visible while connected");

    applyIpConfigPresentation(state, {}, false, false, {}, {}, {},
                              &dhcp, &staticButton, &ip, &mask, &gateway,
                              &apply, unavailable);
    check(!dhcp.isEnabled() && !staticButton.isEnabled() && !apply.isEnabled(),
          "disconnected unsupported controls are inert");

    applyIpConfigPresentation(state, QStringLiteral("flex:A"), true, true,
                              QStringLiteral("10.0.0.2"), QStringLiteral("255.255.255.0"),
                              QStringLiteral("10.0.0.1"), &dhcp, &staticButton,
                              &ip, &mask, &gateway, &apply, unavailable);
    check(staticButton.isChecked() && ip.isEnabled() && ip.text() == QStringLiteral("10.0.0.2"),
          "disconnected-to-Flex resynchronizes and enables static configuration");

    apply.setEnabled(true);
    applyIpConfigPresentation(state, QStringLiteral("flex:A"), true, true,
                              QStringLiteral("changed elsewhere"), {}, {},
                              &dhcp, &staticButton, &ip, &mask, &gateway,
                              &apply, unavailable);
    check(apply.isEnabled() && ip.text() == QStringLiteral("10.0.0.2"),
          "unrelated live refresh preserves pending operator edits");

    applyIpConfigPresentation(state, QStringLiteral("icom:B"), false, false,
                              QStringLiteral("192.0.2.4"), {}, {},
                              &dhcp, &staticButton, &ip, &mask, &gateway,
                              &apply, unavailable);
    check(!apply.isEnabled() && !ip.isEnabled() && ip.text() == QStringLiteral("192.0.2.4"),
          "radio transition clears Apply and replaces stale Flex values");

    return ok ? 0 : 1;
}
