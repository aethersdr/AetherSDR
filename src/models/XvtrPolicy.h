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

// "May this radio tune that band?" for typed VFO entry (and G), net tunes and
// band buttons. Not every gate: activateMemorySpot() calls
// resolveBandStackKey() directly, and the Flex branch ignores declaredBands().
// Branches on the backend, not the band: a Flex command plane uses
// resolveBandStackKey() (FlexLib ModelInfo.cs, no native 440/2 m); any other
// backend is judged against its declared RadioCapabilities
// tuningMinHz/tuningMaxHz (#5041). No declared range means always supported.
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
