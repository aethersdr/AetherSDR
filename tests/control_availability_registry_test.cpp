// #5262 M3a: the three-state control doctrine, as a mechanism.
//
// Two behaviours carry the whole design, and both are things the per-site
// setVisible() plumbing this replaces got wrong:
//
//   1. REGISTRATION APPLIES IMMEDIATELY. A widget built after the connect edge
//      never sees capabilitiesChanged, so it used to sit in whatever state its
//      constructor left it. The Calibration page and DemoApplet both carried
//      hand-written second pushes to paper over that. If registration did not
//      apply, those second pushes would still be necessary — and a pane added
//      by Add Panadapter on a settled session would render wrong indefinitely,
//      because the signal it missed may never fire again.
//
//   2. AN UNAVAILABLE CONTROL IS DIMMED WITH AN ANNOUNCED REASON, never hidden
//      and never silently disabled. A hidden control is not announced at all;
//      a disabled one whose reason lives only in a tooltip is not announced
//      either, because a tooltip is a mouse affordance. That exact shape has
//      regressed twice (#5266 -> #5299, and TX Band / inhibit-during-TUNE), so
//      it is pinned here as well as linted.
//
// Socket-free: a bare RadioModel and plain QWidgets; nothing binds or connects.

#include "TestSettingsProfile.h"
#include "gui/ControlAvailabilityRegistry.h"
#include "core/ThemeManager.h"
#include "core/AppSettings.h"
#include "models/RadioModel.h"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QImage>
#include <QWidget>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

// An injected state source, not a peer: no transport, timers, or firmware model.
class ConnectedBackend final : public IRadioBackend {
public:
    RadioCapabilities capabilities() const override { return {}; }
    bool isConnected() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("control-availability"));
    QApplication app(argc, argv);
    AppSettings::instance().load();
    RadioModel model;                       // not connected
    ControlAvailabilityRegistry registry(model);

    const QString reason = QStringLiteral("Not supported by this radio");

    // ---- permissive while disconnected ----
    //
    // With no radio attached there is nothing to be honest about, and leaving a
    // control dimmed after unplugging reads as a fault rather than as an absent
    // capability. Every gate in applyCapabilitiesToUi behaves this way.
    {
        QLabel w;
        registry.registerWidget(&w, reason,
            [](bool, const RadioCapabilities&) { return false; });
        check(registry.stateOf(&w) != ControlAvailability::Unavailable,
              "a control is not dimmed while disconnected, even if its predicate says no");
        check(w.isEnabled(), "and it stays enabled");
    }

    // ---- registration applies immediately ----
    //
    // The lazy-widget case: this widget is created and registered long after any
    // capabilitiesChanged. If registration did not apply, it would sit in its
    // constructor's state until a signal that may never come.
    {
        QLabel w;
        check(w.accessibleDescription().isEmpty(), "a fresh widget has no description");
        registry.registerWidget(&w, reason,
            [](bool, const RadioCapabilities&) { return true; });
        check(registry.stateOf(&w) == ControlAvailability::Inactive,
              "registration applies at once — no second push needed");
    }

    // ---- the registry drives ENABLED, and never touches visibility ----
    //
    // The doctrine's central claim is "dimmed, not hidden", so the mechanism
    // must express unavailability through setEnabled and leave setVisible
    // alone. Swapping the two is the one mutation that would satisfy every
    // other case here while inverting the whole point — an unavailable control
    // would vanish, which is the pre-M3a behaviour and the accessibility defect.
    //
    // Caught without needing a connected radio: pre-disable the widget, then
    // register it as available. The registry owns the enabled state, so it must
    // come back enabled — a mechanism that drove visibility instead would leave
    // it disabled and invisible to this assertion's intent.
    {
        QLabel w;
        w.setEnabled(false);
        const bool hiddenBefore = w.isHidden();
        registry.registerWidget(&w, reason,
            [](bool, const RadioCapabilities&) { return true; });
        check(w.isEnabled(),
              "the registry drives the ENABLED state, not visibility");
        check(w.isHidden() == hiddenBefore,
              "and never changes whether the control is shown — dimmed, not hidden");
    }

    // A retained engaged flag must not keep a disconnected control colored.
    {
        QLabel w;
        registry.registerWidget(&w, reason,
            [](bool, const RadioCapabilities&) { return true; }, [] { return true; });
        check(registry.stateOf(&w) == ControlAvailability::Inactive,
              "disconnect is inactive even when the owner retains an engaged flag");
    }
    model.setBackendForTest(std::make_unique<ConnectedBackend>(), QStringLiteral("test"));

    // ---- available + engaged ----
    {
        QLabel w;
        bool engaged = true;
        registry.registerWidget(&w, reason,
            [](bool, const RadioCapabilities&) { return true; },
            [&engaged] { return engaged; });
        check(registry.stateOf(&w) == ControlAvailability::Active,
              "a supported, engaged control is Active");
        engaged = false;
        registry.refreshEngaged();
        check(registry.stateOf(&w) == ControlAvailability::Inactive,
              "and becomes Inactive when it disengages, without being disabled");
        check(w.isEnabled(), "an inactive control stays usable — it is not unavailable");
    }

    // ---- a QAction carries its reason where Qt announces it ----
    //
    // QAction has no accessibleDescription; Qt exposes its status tip to
    // accessibility clients. Setting only a tooltip is the regression that
    // shipped on TX Band Settings and Inhibit-during-TUNE.
    {
        QAction a(QStringLiteral("TX Band Settings…"));
        registry.registerAction(&a, reason,
            [](bool, const RadioCapabilities&) { return true; });
        check(a.isEnabled(), "a supported action is enabled");
        check(a.statusTip().isEmpty(),
              "and carries no unavailability reason while it is supported");
    }

    // ---- dead widgets do not accumulate ----
    {
        const int before = registry.registrationCount();
        {
            QLabel scoped;
            registry.registerWidget(&scoped, reason,
                [](bool, const RadioCapabilities&) { return true; });
        }
        registry.refreshEngaged();          // prunes on the way through
        check(registry.registrationCount() <= before,
              "a destroyed widget is pruned rather than leaking a QPointer entry");
    }

    // Connected widgets exercise the path the old disconnected-only test missed.
    ThemeManager& theme = ThemeManager::instance();
    for (const QString& themeName : {QStringLiteral("Default Dark"),
                                     QStringLiteral("Default Light")}) {
        check(theme.setActiveTheme(themeName), "the shipped theme loads");
        bool supported = false;
        bool engaged = false;
        QPushButton button(QStringLiteral("Capability"));
        button.setObjectName(QStringLiteral("availabilityButton"));
        button.setCheckable(true);
        button.setChecked(true);
        const QString baseStyle = QStringLiteral(
            "QPushButton#availabilityButton:checked { color: {{color.text.primary}}; "
            "padding: 7px; border: 2px solid {{color.border.strong}}; }");
        theme.applyStyleSheet(&button, baseStyle);
        button.show();
        QApplication::processEvents();
        const QColor baseForeground = button.palette().color(QPalette::ButtonText);
        const QImage basePixels = button.grab().toImage();
        registry.registerWidget(&button, reason,
            [&supported](bool, const RadioCapabilities&) { return supported; },
            [&engaged] { return engaged; });
        QAction action(QStringLiteral("Capability action"));
        registry.registerAction(&action, reason,
            [&supported](bool, const RadioCapabilities&) { return supported; });
        QApplication::processEvents();
        check(registry.stateOf(&button) == ControlAvailability::Unavailable,
              "late registration sees a connected unsupported capability immediately");
        check(!button.isEnabled() && !button.isHidden(), "unavailable stays visible and disabled");
        check(button.accessibleDescription() == reason && button.toolTip() == reason,
              "unavailable widget carries both reason channels");
        check(!action.isEnabled() && action.statusTip() == reason && action.toolTip() == reason,
              "unavailable action carries its reason and cannot activate");
        check(button.palette().color(QPalette::ButtonText) == theme.color("color.control.unavailable"),
              "unavailable foreground overrides a styled checked button with an ID selector");

        supported = true;
        registry.refreshEngaged();
        QApplication::processEvents();
        check(button.isEnabled() && action.isEnabled() && action.statusTip().isEmpty(),
              "supported recovery clears the action reason and enables both controls");
        check(button.toolTip().isEmpty()
                  && button.accessibleDescription() == QStringLiteral("Available, not currently active"),
              "inactive clears stale reason and announces its own state");
        check(button.palette().color(QPalette::ButtonText) == theme.color("color.control.inactive"),
              "inactive text uses the enabled foreground role");
        engaged = true;
        registry.refreshEngaged();
        QApplication::processEvents();
        check(button.styleSheet() == theme.resolve(baseStyle)
                  && button.accessibleDescription().isEmpty(),
              "active restores the original complete stylesheet");
        check(button.palette().color(QPalette::ButtonText) == baseForeground
                  && button.grab().toImage() == basePixels,
              "active restores the base foreground and rendered pixels");

        // Both current theme changes and base-style replacements must compose.
        supported = false;
        registry.refreshEngaged();
        const QColor edited(12, 123, 234);
        theme.setColor(QStringLiteral("color.control.unavailable"), edited);
        QApplication::processEvents();
        check(button.palette().color(QPalette::ButtonText) == edited,
              "token edits repaint unavailable controls without capability changes");
        theme.applyStyleSheet(&button, QStringLiteral("QPushButton { color: {{color.accent}}; padding: 9px; }"));
        QApplication::processEvents();
        check(button.palette().color(QPalette::ButtonText) == edited,
              "replacing the base style preserves the treatment");
        supported = true;
        engaged = true;
        registry.refreshEngaged();
        QApplication::processEvents();
        check(button.palette().color(QPalette::ButtonText) == theme.color("color.accent"),
              "active restores the latest base style, not a stale snapshot");

        // The signal's disconnect snapshot wins over an independently polled backend.
        supported = false;
        registry.refreshEngaged();
        emit model.capabilitiesChanged(false, {});
        check(button.isEnabled() && action.isEnabled(), "disconnect payload is consumed directly");
        check(button.toolTip().isEmpty() && action.statusTip().isEmpty(), "disconnect clears stale reasons");
        check(registry.stateOf(&button) == ControlAvailability::Inactive,
              "disconnect always returns to Inactive even with retained engagement");
    }
    {
        bool engaged = false;
        QLabel label(QStringLiteral("Availability text"));
        label.resize(240, 40);
        label.show();
        registry.registerWidget(&label, reason,
            [](bool, const RadioCapabilities&) { return true; }, [&engaged] { return engaged; });
        QApplication::processEvents();
        const QString otherTheme = theme.activeTheme() == QStringLiteral("Default Light")
            ? QStringLiteral("Default Dark") : QStringLiteral("Default Light");
        check(theme.setActiveTheme(otherTheme), "switch theme with a registered inactive label");
        QApplication::processEvents();
        check(label.palette().color(QPalette::WindowText) == theme.color("color.control.inactive"),
              "theme switch refreshes an unstyled label treatment");
        const QImage newInactive = label.grab().toImage();
        engaged = true;
        registry.refreshEngaged();
        QApplication::processEvents();
        check(newInactive != label.grab().toImage(), "inactive and active render different pixels");
        check(label.styleSheet().isEmpty(), "active unstyled label resumes style inheritance");
        QLineEdit edit;
        theme.applyStyleSheet(&edit, QStringLiteral("color: {{color.accent}}; padding: 3px;"));
        edit.show();
        registry.registerWidget(&edit, reason,
            [](bool, const RadioCapabilities&) { return false; });
        QApplication::processEvents();
        check(edit.palette().color(QPalette::Text) == theme.color("color.control.unavailable"),
              "unavailable line edits use the Text role");
    }

    if (g_failures == 0) {
        std::printf("control_availability_registry_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
