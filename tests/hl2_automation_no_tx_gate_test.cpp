// AETHER_AUTOMATION_NO_TX closes the HL2's own transmit gate, not only the bridge's.
//
// In an automation run the HL2 decides its gate once, at construction, from the
// same launch inputs the bridge reads: ALLOW_TX, the persisted "Allow TX via MCP"
// toggle, and NO_TX, which pins the bridge off over both. Each row constructs a
// backend under one combination and reads the gate back from both layers: the
// declared canTransmit (what RadioModel refuses on) and MetisClient's wire gate.
// Constructed backend only: no connectRadio(), socket, peer or DSP. Never keys.

#include "TestSettingsProfile.h"
#include "core/AutomationBridgeSettings.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisClient.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <cstdio>
#include <string>

namespace AetherSDR::hl2 {
// Reuses an existing friend rather than adding a production test hook.
struct Hl2TxGateTestAccess {
    static bool wireGateOpen(Hl2Backend& backend)
    {
        bool open = true;
        QMetaObject::invokeMethod(backend.m_metis, [&open, metis = backend.m_metis] {
            open = metis->transmitEnabled();
        }, Qt::BlockingQueuedConnection);
        return open;
    }
};
} // namespace AetherSDR::hl2

using AetherSDR::AutomationBridgeSettings;
using AetherSDR::hl2::Hl2Backend;
using AetherSDR::hl2::Hl2TxGateTestAccess;

namespace {

int failures = 0;

void check(bool condition, const std::string& what)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!condition) {
        ++failures;
    }
}

void setEnv(const char* name, bool on)
{
    if (on) {
        qputenv(name, "1");
    } else {
        qunsetenv(name);
    }
}

struct Row {
    const char* name;
    bool automation;
    bool allowTxEnv;
    bool persistedTxAllowed;
    bool noTx;
    bool expectTransmit;
};

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-automation-no-tx-gate"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    Hl2Backend::setHostOffersTransmit(true);

    // The first two automation rows are positive controls: without them a gate
    // that never opened would pass every NO_TX row below.
    const Row rows[] = {
        {"automation, ALLOW_TX", true, true, false, false, true},
        {"automation, persisted Allow TX via MCP", true, false, true, false, true},
        {"automation, neither", true, false, false, false, false},
        {"automation, ALLOW_TX and NO_TX", true, true, false, true, false},
        {"automation, persisted Allow TX via MCP and NO_TX", true, false, true, true, false},
        {"automation, ALLOW_TX, persisted and NO_TX", true, true, true, true, false},
        // NO_TX pins the bridge; the operator's own keys in an interactive run stay.
        {"interactive, NO_TX", false, false, false, true, true},
    };

    for (const Row& row : rows) {
        setEnv("AETHER_AUTOMATION", row.automation);
        setEnv("AETHER_AUTOMATION_ALLOW_TX", row.allowTxEnv);
        setEnv("AETHER_AUTOMATION_NO_TX", row.noTx);
        AutomationBridgeSettings::setTxAllowed(row.persistedTxAllowed);

        Hl2Backend backend;
        const bool declared = backend.capabilities().canTransmit;
        const bool wire = Hl2TxGateTestAccess::wireGateOpen(backend);
        const std::string expect = row.expectTransmit ? "open" : "closed";
        check(declared == row.expectTransmit,
              std::string(row.name) + ": canTransmit is " + expect);
        check(wire == row.expectTransmit,
              std::string(row.name) + ": the MetisClient wire gate is " + expect);
        if (row.automation) {
            check(AutomationBridgeSettings::launchTxAllowed() == declared,
                  std::string(row.name) + ": the bridge's launch answer agrees with the HL2 gate");
        }
    }

    setEnv("AETHER_AUTOMATION", false);
    setEnv("AETHER_AUTOMATION_ALLOW_TX", false);
    setEnv("AETHER_AUTOMATION_NO_TX", false);
    AutomationBridgeSettings::setTxAllowed(false);

    if (failures == 0) {
        std::printf("hl2_automation_no_tx_gate_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
