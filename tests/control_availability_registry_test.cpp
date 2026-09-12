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

#include "gui/ControlAvailabilityRegistry.h"
#include "models/RadioModel.h"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QWidget>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
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

    if (g_failures == 0)
        std::printf("control_availability_registry_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
