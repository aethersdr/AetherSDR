// The Hermes-Lite 2 keeps its spots in this client, because it has nowhere
// else to put them.
//
// On a Flex, every DX-cluster, RBN, WSJT-X, POTA and manual spot is published
// as `spot add` wire text, and it is the radio's `spot <id>` status coming back
// that places it on the panadapter. The HL2 has no command plane:
// RadioModel::sendCmd drops that text at its `!hasCommandPlane()` branch and no
// status ever arrives. With alwaysUseClientSideSpots left false the spot feeds
// connected, fetched and drew nothing. True sends them down the passive-local
// SpotModel route instead — the one Icom already takes for the same reason.
//
// SOCKET-FREE. Constructs a backend and reads capabilities(); binds nothing,
// connects nothing, pumps no event loop, and reaches no radio.
//
// WHAT THIS FILE CANNOT OBSERVE:
//   1. The struct default is false, the opposite of the value asserted, so
//      deleting the declaration fails here directly. A second assignment
//      later in capabilities() is also caught, because this reads the value
//      the function returns, not the source text.
//   2. That MainWindow actually honours the field. passive_spots_policy_test
//      pins the call sites and SpotCommandPolicy's truth table; reaching the
//      real SpotHub-to-SpotModel path needs a constructed MainWindow, and no
//      test here builds one. Nothing here was measured on a radio.

#include "TestSettingsProfile.h"
#include "core/SpotCommandPolicy.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/hl2/Hl2Backend.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool condition, const char* label)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", label);
    if (!condition) {
        ++failures;
    }
}
}  // namespace

int main(int argc, char** argv)
{
    // The backend touches AppSettings on construction. Redirect it before
    // QCoreApplication so nothing here can read or write the operator's live
    // configuration.
    TestSettingsProfile settingsProfile(QStringLiteral("hl2-client-side-spots-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);

    hl2::Hl2Backend backend;
    const RadioCapabilities caps = backend.capabilities();

    // No `caps.family` assertion, per the rule in
    // docs/architecture/radio-capabilities-map.md: every assertion reads a
    // capability. The constructed hl2::Hl2Backend is what makes this the HL2.

    check(caps.alwaysUseClientSideSpots,
          "HL2 declares alwaysUseClientSideSpots (no command plane for `spot add`)");

    // The consequence, through the production policy rather than a copy of it:
    // with the operator's Passive toggle at its default (off), the HL2 must
    // still not be handed `spot add` commands to drop.
    check(!SpotCommandPolicy::passiveSpotsModeEnabled(),
          "isolated profile starts with Passive spots mode off");
    check(!SpotCommandPolicy::shouldSendSpotAddCommands(caps.alwaysUseClientSideSpots),
          "HL2 spots take the passive-local SpotModel route with Passive off");

    return failures == 0 ? 0 : 1;
}
