#pragma once

#include "RadioSettingsScope.h"
#include "backends/RadioCapabilities.h"
#include "backends/RestoredRadioState.h"

namespace AetherSDR {

// Client-side memory for radios with none of their own (RFC #4603). Engagement
// is capability-shaped: the radio's ClientSettingsDomains says which domains the
// client persists and restores; empty (Flex, Sim) leaves this inert.
// store() refuses to overwrite a document whose schema_version exceeds
// kSchemaVersion (a rebuild would drop unknown fields), and writes only the
// DECLARED domains, so a narrower declaration erases the others. Memories is
// not gated here; the host memory bank (#4590) has its own documents.
namespace RadioStateMemory {

// The feature document name in radio_settings, and its current schema.
inline QString featureName() { return QStringLiteral("OperatingState"); }
constexpr int kSchemaVersion = 2;

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

// Explicit RFC #5468 cutover helpers for the RTL accepted-state owner. Exact rows
// only; the new owner must claim successfully before overlapping old-domain
// capture is disabled. The legacy document stays as a downgrade snapshot.
struct RtlMigrationSource {
    AppSettings::FeatureReadStatus status = AppSettings::FeatureReadStatus::Unavailable;
    RestoredRadioState state;
};
RtlMigrationSource rtlMigrationSource(const RadioSettingsScope& scope);
bool storeRtlRfGainPreservingLegacy(const RadioSettingsScope& scope,
                                    const RestoredRadioState& state);

} // namespace RadioStateMemory

} // namespace AetherSDR
