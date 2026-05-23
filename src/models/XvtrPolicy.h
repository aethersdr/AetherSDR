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

BandStackKeyResult resolveBandStackKey(const QString& bandName,
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

} // namespace AetherSDR::XvtrPolicy
