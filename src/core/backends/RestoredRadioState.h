#pragma once

#include <QJsonObject>
#include <QString>

namespace AetherSDR {

// The typed restore contract of RFC #4603 proposal B: what the client remembers
// about a radio whose ClientSettingsDomains make the client its memory, handed to
// the backend BEFORE connect (IRadioBackend::applyRestoredState). Typed universal
// fields plus a per-family extension document that only the owning backend
// writes, reads and validates; RadioStateMemory round-trips it opaquely. A
// zero/empty field means "not restored". Restoring NEVER keys transmit.
struct RestoredRadioState {
    // Universal — gated per-domain by RadioCapabilities::clientSettingsDomains
    double rfFrequencyHz = 0.0;   // Tuning
    QString mode;                 // Tuning
    double filterLowHz = 0.0;     // Passband
    double filterHighHz = 0.0;    // Passband
    int sampleRateHz = 0;         // SpanRate

    // Agc. Persisted here because on a radio without its own AGC it lives in the
    // host's DSP (#4909). The threshold sentinel is -1, NOT 0: zero is a selectable
    // AGC-T, so "not restored" needs a value outside the 0..100 range.
    QString agcMode;              // Agc — "off" | "slow" | "med" | "fast"
    int agcThreshold = -1;        // Agc — 0..100 OPERATOR UNITS, not dB; -1 = not
                                  // restored. Deliberately NOT "…Db": the backend
                                  // multiplies by its own ceiling-per-unit to reach
                                  // real dB, and KiwiSdrClient has a genuine
                                  // agcThresholdDb nearby. Matches
                                  // SliceDelta::agcThreshold, the same 0..100 scale.

    // CW controls. Flex persists and reports these in the radio; a host-keyed
    // backend has no such authority, so a backend declaring the Cw domain makes
    // the client its radio-scoped memory. Sentinel values distinguish an older
    // document with no CW section from deliberate zero/false selections.
    int cwSpeed = 0;              // Cw — 5..100 WPM; 0 = not restored
    int cwPitch = 0;              // Cw — 100..6000 Hz; 0 = not restored
    int cwBreakIn = -1;           // Cw — 0/1; -1 = not restored
    int cwDelay = -1;             // Cw — 0..2000 ms; -1 = not restored
    int cwSidetone = -1;          // Cw — 0/1; -1 = not restored
    int cwIambic = -1;            // Cw — 0/1; -1 = not restored
    int cwIambicMode = -1;        // Cw — 0=A, 1=B; -1 = not restored
    int cwSwapPaddles = -1;       // Cw — 0/1; -1 = not restored
    int cwlEnabled = -1;          // Cw — 0/1; -1 = not restored
    int monGainCw = -1;           // Cw — 0..100; -1 = not restored
    int monPanCw = -1;            // Cw — 0..100; -1 = not restored

    // Per-family extension document (per-band gain/drive maps), versioned by its
    // owner. GATED PER DOMAIN: the engine hands over only the sub-objects named for
    // declared domains ("rfGain" for RfGain, "txSetpoints" for TxSetpoints); their
    // contents are opaque above the seam and validated by the owning backend.
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
               && cwSpeed == 0 && cwPitch == 0 && cwBreakIn < 0
               && cwDelay < 0 && cwSidetone < 0 && cwIambic < 0
               && cwIambicMode < 0 && cwSwapPaddles < 0 && cwlEnabled < 0
               && monGainCw < 0 && monPanCw < 0
               && extension.isEmpty();
    }
};

} // namespace AetherSDR
