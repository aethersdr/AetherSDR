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

    // Agc. The AGC lives in the HOST's DSP for a radio with no AGC of its own,
    // so the operator's choice has nowhere to live but here — without it every
    // launch reopened the WDSP channel on Config's construction defaults
    // ("med" / 65) and silently discarded the setting (#4909).
    //
    // The threshold sentinel is -1, NOT 0: zero is a legitimate AGC-T the
    // operator can select, so "not restored" needs a value outside the 0..100
    // control range or a deliberate 0 would be indistinguishable from an
    // absent field and would round-trip into the default.
    QString agcMode;              // Agc — "off" | "slow" | "med" | "fast"
    int agcThreshold = -1;        // Agc — 0..100 OPERATOR UNITS, not dB; -1 = not
                                  // restored. Deliberately NOT "…Db": the backend
                                  // multiplies by its own ceiling-per-unit to reach
                                  // real dB, and KiwiSdrClient has a genuine
                                  // agcThresholdDb nearby. Matches
                                  // SliceDelta::agcThreshold, the same 0..100 scale.

    // Per-family extension document (per-band gain/drive maps live here —
    // RFC PR 3). Versioned by its owner. GATED PER DOMAIN at the top level:
    // the engine hands over only the sub-objects named for declared domains —
    // "rfGain" (ClientSettingsDomain::RfGain) and "txSetpoints"
    // (ClientSettingsDomain::TxSetpoints) — and each sub-object's CONTENTS
    // stay opaque to everything above the seam; the owning backend writes and
    // validates them (Principle VII; PR #4614 review).
    int extensionSchemaVersion = 0;
    QJsonObject extension;

    // "This radio has no memory." Load returns it for an undeclared or empty
    // domain set, and radio_state_memory_test pins that — so EVERY field added
    // above has to be represented here or a document carrying only the new
    // field would report itself as nothing stored. The AGC threshold is why
    // that is worth spelling out: its absent value is -1, not 0.
    bool isEmpty() const
    {
        return rfFrequencyHz == 0.0 && mode.isEmpty() && filterLowHz == 0.0
               && filterHighHz == 0.0 && sampleRateHz == 0
               && agcMode.isEmpty() && agcThreshold < 0
               && extension.isEmpty();
    }
};

} // namespace AetherSDR
