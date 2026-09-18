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

    // #5262 M3a reversed the doctrine these assertions used to pin. An
    // individual control is DIMMED WITH A REASON, never hidden: a hidden
    // control is not announced to a screen reader at all, so there is no way to
    // learn the radio simply does not support it. #5266 shipped the disabled
    // state + tooltip + accessibleDescription on the Enforce Private IP button
    // and #5299 replaced all three with a bare setVisible() four days later —
    // which this test then locked in.
    const QString why = QStringLiteral("Not supported by this radio");
    // Shown first: a QWidget that has never been shown reports isHidden()
    // regardless, so an absolute assertion would pass whatever the code did.
    QWidget gatedSurface;
    gatedSurface.show();
    applyCapabilitySurfaceAvailability(&gatedSurface, true, false, why);
    check(!gatedSurface.isHidden(),
          "an unsupported control stays VISIBLE while connected — dimmed, not hidden");
    check(!gatedSurface.isEnabled(),
          "and is disabled");
    check(gatedSurface.accessibleDescription() == why,
          "and states the reason on the channel a screen reader reads");
    check(gatedSurface.toolTip() == why,
          "and on the tooltip for sighted users");

    applyCapabilitySurfaceAvailability(&gatedSurface, false, false, why);
    check(gatedSurface.isEnabled() && gatedSurface.accessibleDescription().isEmpty(),
          "permissive on disconnect: enabled again, with no stale reason");

    applyCapabilitySurfaceAvailability(&gatedSurface, true, true, why);
    check(gatedSurface.isEnabled() && gatedSurface.toolTip().isEmpty(),
          "a supported control carries no unavailability reason");

    // The one sanctioned hide: a cohesive radio-specific CLUSTER, at group
    // granularity rather than per control.
    QWidget gatedCluster;
    gatedCluster.show();
    applyCapabilityClusterVisibility(&gatedCluster, true, false);
    check(gatedCluster.isHidden(),
          "an unsupported cohesive cluster may still hide wholesale");
    applyCapabilityClusterVisibility(&gatedCluster, false, false);
    check(!gatedCluster.isHidden(),
          "and is permissive on disconnect like everything else");

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
