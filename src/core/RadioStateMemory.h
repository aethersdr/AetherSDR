#pragma once

#include "RadioSettingsScope.h"
#include "backends/RadioCapabilities.h"
#include "backends/RestoredRadioState.h"

namespace AetherSDR {

// The client-side memory for radios that have none of their own (RFC #4603
// proposal B). Engagement is decided by ONE rule and it is capability-shaped,
// never family-shaped: a radio's declared ClientSettingsDomains says which
// domains the client persists and restores. Empty (Flex, Sim) ⇒ this class is
// inert and the radio-authoritative policy (Constitution II/III) is untouched.
//
// PR 2 scope (this file): the store/load round-trip of the operating-state
// feature document and the domain gating. The live capture wiring (model
// deltas → debounced store) and the per-domain field application — including
// the per-band drive/LNA maps from nigelfenton's RFC review — land in PR 3
// together with the HL2 restore path.
namespace RadioStateMemory {

// The feature document name in radio_settings, and its current schema.
inline QString featureName() { return QStringLiteral("OperatingState"); }
constexpr int kSchemaVersion = 1;

// The single engagement predicate: restore/capture happen only for declared
// domains. Deliberately a function so tests and call sites share one truth.
inline bool shouldEngage(const RadioCapabilities& caps)
{
    return caps.clientSettingsDomains != RadioCapabilities::ClientSettingsDomains{};
}

// Load the remembered operating state for a radio, filtered to the domains
// its backend declares. Undeclared domains come back as "not restored" even
// if an older document carries them (a capability downgrade must not smuggle
// state past the gate). Returns an empty state when nothing is stored.
RestoredRadioState load(const RadioSettingsScope& scope,
                        const RadioCapabilities& caps);

// Store the operating state as one atomic feature document, filtered to the
// declared domains. A state that is empty after filtering is not written.
bool store(const RadioSettingsScope& scope, const RadioCapabilities& caps,
           const RestoredRadioState& state);

} // namespace RadioStateMemory

} // namespace AetherSDR
