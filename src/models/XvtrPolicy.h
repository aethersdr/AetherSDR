#pragma once

#include "ModelCapabilities.h"

#include <QString>
#include <QVector>

namespace AetherSDR::XvtrPolicy {

struct Transverter {
    int     index{0};
    int     order{-1};
    QString name;
    double  rfFreqMhz{0.0};
    double  ifFreqMhz{0.0};
    bool    isValid{false};
};

struct BandStackKeyResult {
    QString key;
    QString unsupportedReason;

    bool isSupported() const { return !key.isEmpty(); }
};

// One answer to "may this radio tune that band?", shared by the three
// non-Flex band gates: typed VFO entry (with the G shortcut behind it),
// net tunes, and the band buttons. NOT every gate in the app —
// activateMemorySpot() still calls resolveBandStackKey() raw, and the Flex
// branch here does not consult declaredBands(), so a #4027 gateway that
// declares 440 gets a band button but still meets the #5041 sentence on a
// typed frequency.
//
// resolveBandStackKey() below only ever answers the FLEX question — its native
// table is FlexLib's ModelInfo.cs, which has no 440 entry because no Flex
// covers UHF without a transverter (Principle I). Asking it about an IC-705
// produced the #5041 refusal: "Band 440 requires a configured XVTR", on a radio
// that reaches 70 cm natively and has no `display pan band=` plane for the key
// to be written to in the first place. 2 m refused identically, since
// ModelCapabilities is also a Flex model table.
//
// So the branch is the backend's, not the band's: a radio with a Flex command
// plane keeps the band-stack resolution unchanged; anything else is judged
// against the tuning range its own backend declared (RadioCapabilities
// tuningMinHz/tuningMaxHz — populated for Icom from the per-model row). A
// backend that declares no range keeps the previous unconditional behaviour.
struct BandTuneAdmissibility {
    bool    supported{false};
    QString bandStackKey;  // Flex `display pan band=` key; empty off a Flex.
    // Why it was refused, as a LOG line — this layer has no QObject to hang
    // tr() on, and a log should not be translated anyway. What the operator
    // reads is composed by bandTuneRefusalText() in MainWindowHelpers, which is
    // the single translatable copy of that sentence and the reason the fields
    // below are typed rather than pre-formatted: the band buttons and the typed
    // tune must not word the same refusal two different ways.
    QString reason;        // empty when supported
    bool    outsideTuningRange{false};  // refused by RANGE, not by band stack
    double  rangeMinMhz{0.0};
    double  rangeMaxMhz{0.0};
};

struct WaterfallTileRange {
    double lowMhz{0.0};
    double highMhz{0.0};
    bool   shifted{false};
};

struct WaterfallTileMatch {
    bool    matched{false};
    int     index{-1};
    int     order{-1};
    QString name;
    double  observedOffsetMhz{0.0};
    double  expectedOffsetMhz{0.0};
    double  toleranceMhz{0.0};
};

struct MaxPowerRange {
    double minimumDbm{-10.0};
    double maximumDbm{15.0};
};

BandStackKeyResult resolveBandStackKey(const QString& bandName,
                                       const QVector<Transverter>& xvtrs,
                                       ModelCapabilities caps = {});

BandTuneAdmissibility evaluateBandTune(bool usesFlexCommandPlane,
                                       const QString& bandName,
                                       double targetMhz,
                                       double tuningMinHz,
                                       double tuningMaxHz,
                                       const QVector<Transverter>& xvtrs,
                                       ModelCapabilities caps = {});

bool isWaterfallTileOutsidePan(double lowMhz, double highMhz, double panCenterMhz);

WaterfallTileMatch matchWaterfallTileTransverterOffset(double lowMhz, double highMhz,
                                                       double panCenterMhz,
                                                       const QVector<Transverter>& xvtrs);

bool waterfallTileMatchesTransverterOffset(double lowMhz, double highMhz,
                                           double panCenterMhz,
                                           const QVector<Transverter>& xvtrs);

WaterfallTileRange mapWaterfallTileRange(double lowMhz, double highMhz,
                                         double panCenterMhz,
                                         const QVector<Transverter>& xvtrs,
                                         bool hasXvtrSliceAntenna);

MaxPowerRange maxPowerRangeFor(double ifFreqMhz, const QString& radioModel);
double clampMaxPowerDbm(double maxPowerDbm, double ifFreqMhz, const QString& radioModel);

} // namespace AetherSDR::XvtrPolicy
