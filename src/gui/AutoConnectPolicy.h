#pragma once

// Pure, UI-free decision helpers for saved-radio autoconnect and client-slot
// contention. Shared by the three discovery paths and confirmClientSlotAvailability
// in MainWindow so the rules live in one place, and unit-tested without a running
// MainWindow or any Qt dialog (#4401).
namespace aether {

// Saved-radio autoconnect proceeds only when the process-scoped override
// (AETHER_AUTOMATION_NO_AUTOCONNECT) is unset AND the AutoConnectToLastRadio
// setting is enabled. Keeping the override lets an automation/CI launch stay
// idle for one process without flipping the persistent setting — which would
// also change normal interactive behavior.
inline bool savedRadioAutoConnectAllowed(bool automationNoAutoConnectOverride,
                                         bool autoConnectSettingEnabled)
{
    return !automationNoAutoConnectOverride && autoConnectSettingEnabled;
}

// Outcome of a connect attempt against a radio that may already have clients.
enum class ClientSlotAction {
    Connect,  // a slot is freely available — connect without prompting
    Decline,  // contended, but headless under the automation bridge: decline
              // rather than block on a modal or evict another client
    Prompt,   // contended and interactive: ask the operator to resolve it
};

// multiClientCapable: local multiFLEX enabled, or WAN licensedClients > 1.
// connectedClients:   clients already on the radio (our own excluded upstream).
// maxSlices:          the model's client-slot limit (>= 1).
// automation:         running under the AETHER_AUTOMATION bridge (no operator).
//
// A slot is "contended" when a single-client radio already has a client, or the
// model's slots are full. Contended + headless declines (never auto-evicts a
// peer); contended + interactive prompts the operator; otherwise connect.
inline ClientSlotAction resolveClientSlotAction(bool multiClientCapable,
                                                int connectedClients,
                                                int maxSlices,
                                                bool automation)
{
    const bool contended =
        (!multiClientCapable && connectedClients > 0)
        || (connectedClients >= maxSlices);
    if (!contended)
        return ClientSlotAction::Connect;
    return automation ? ClientSlotAction::Decline : ClientSlotAction::Prompt;
}

} // namespace aether
