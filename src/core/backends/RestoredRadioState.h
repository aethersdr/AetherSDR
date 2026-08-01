#pragma once

#include <QJsonObject>
#include <QString>

namespace AetherSDR {

// The typed restore contract of RFC #4603 proposal B: what the client's
// settings store remembers about a radio whose declared ClientSettingsDomains
// make the client its memory. Handed to the backend BEFORE connect
// (IRadioBackend::applyRestoredState) so the connect/pushInitialState path can
// bring the radio up where the operator left it.
//
// Shape follows the aetherd 2.3 universal/extension field classification:
// typed fields for state every family shares, plus a per-family extension
// document that ONLY the owning backend writes, reads, and validates
// (Principle VII — boundary input validation lives with the owner). Generic
// engine code (RadioStateMemory) round-trips the extension opaquely and must
// never interpret it.
//
// A zero/empty field means "not restored" — the backend keeps its own default.
// Restoring NEVER keys transmit (Principle VI): the struct carries setpoints,
// and the TX gate is untouched.
struct RestoredRadioState {
    // Universal — gated per-domain by RadioCapabilities::clientSettingsDomains
    double rfFrequencyHz = 0.0;   // Tuning
    QString mode;                 // Tuning
    double filterLowHz = 0.0;     // Passband
    double filterHighHz = 0.0;    // Passband
    int sampleRateHz = 0;         // SpanRate

    // Per-family extension document (per-band gain/drive maps live here —
    // RFC PR 3). Versioned by its owner. GATED PER DOMAIN at the top level:
    // the engine hands over only the sub-objects named for declared domains —
    // "rfGain" (ClientSettingsDomain::RfGain) and "txSetpoints"
    // (ClientSettingsDomain::TxSetpoints) — and each sub-object's CONTENTS
    // stay opaque to everything above the seam; the owning backend writes and
    // validates them (Principle VII; PR #4614 review).
    int extensionSchemaVersion = 0;
    QJsonObject extension;

    bool isEmpty() const
    {
        return rfFrequencyHz == 0.0 && mode.isEmpty() && filterLowHz == 0.0
               && filterHighHz == 0.0 && sampleRateHz == 0
               && extension.isEmpty();
    }
};

} // namespace AetherSDR
