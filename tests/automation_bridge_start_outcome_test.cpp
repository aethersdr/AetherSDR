// Pins AutomationBridgeSettings::recordStartOutcome() — the policy that decides
// what the saved Agent-Automation opt-in says after an ASYNCHRONOUS bridge start
// resolves (#4181). The GUI half (MainWindow's token callback and the Radio
// Setup toggle) is not linkable here; this is the socket-free seam it calls.
//
// Four outcomes matter, and two of them regress silently if the policy drifts:
//   enable → bind ok        : opt-in recorded true
//   enable → bind failed    : opt-in cleared, so a doomed start is not retried
//                             every launch with nobody told
//   env-forced, either way  : the saved opt-in is NOT ours — left untouched
// No socket is opened: the bind result is injected as a bool.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AutomationBridgeSettings.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR;

namespace {

int fail(const char* what)
{
    std::fprintf(stderr, "automation_bridge_start_outcome_test: %s\n", what);
    return 1;
}

} // namespace

int main(int argc, char* argv[])
{
    TestSettingsProfile profile(QStringLiteral("automation-bridge-start-outcome"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    // Fresh store: nothing saved, nothing forced.
    qunsetenv("AETHER_AUTOMATION");
    if (AutomationBridgeSettings::enabled()) {
        return fail("fresh store must start with the bridge disabled");
    }
    if (AutomationBridgeSettings::envForced()) {
        return fail("envForced() must be false without AETHER_AUTOMATION");
    }

    // Operator enable, bind succeeds → persisted true.
    if (!AutomationBridgeSettings::recordStartOutcome(true, false)
        || !AutomationBridgeSettings::enabled()) {
        return fail("a successful operator start must persist enabled=true");
    }

    // Persisted opt-in, next launch's bind fails → cleared (#4181: otherwise
    // every launch silently re-attempts the doomed start).
    if (AutomationBridgeSettings::recordStartOutcome(false, false)
        || AutomationBridgeSettings::enabled()) {
        return fail("a failed operator start must clear the persisted opt-in");
    }

    // Env-forced start: the saved opt-in is not ours to rewrite, on either
    // outcome. Seed it true, fail forced → still true; seed false, succeed
    // forced → still false.
    AutomationBridgeSettings::setEnabled(true);
    if (!AutomationBridgeSettings::recordStartOutcome(false, true)
        || !AutomationBridgeSettings::enabled()) {
        return fail("a failed env-forced start must not clear the saved opt-in");
    }
    AutomationBridgeSettings::setEnabled(false);
    if (AutomationBridgeSettings::recordStartOutcome(true, true)
        || AutomationBridgeSettings::enabled()) {
        return fail("a successful env-forced start must not fake an opt-in");
    }

    // envForced() reads the launch override the GUI passes through.
    qputenv("AETHER_AUTOMATION", "1");
    if (!AutomationBridgeSettings::envForced()) {
        return fail("envForced() must be true with AETHER_AUTOMATION set");
    }
    qunsetenv("AETHER_AUTOMATION");

    // The sibling bools ride in the same nested document and must survive the
    // read-modify-write (Principle V: one atomic object).
    AutomationBridgeSettings::setReadOnly(true);
    AutomationBridgeSettings::recordStartOutcome(true, false);
    if (!AutomationBridgeSettings::readOnly() || !AutomationBridgeSettings::enabled()) {
        return fail("recording an outcome must not clobber sibling bridge fields");
    }
    return 0;
}
