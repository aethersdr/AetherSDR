#pragma once

#include "core/SharedCapturePolicy.h"
#include <QByteArray>
#include <QMetaType>
#include <cmath>

namespace AetherSDR {

// Optional wider coverage from the SAME observation as spectrumFrameReady's
// viewport bins, in the SAME level units. Host-endian float32 like that frame.
// Bounds describe the genuine usable samples, never a requested display span.
// Empty preserves the existing viewport-derived waterfall contract. Carrying
// samples and RF bounds together prevents a queued frame inheriting a later
// capture's geometry. Producers still reject obsolete acquisition deliveries.
// Both arrays are final display observations: their producer owns temporal
// estimation. Consumers preserve those samples rather than adding another EMA.
struct SpectrumCoverage {
    QByteArray bins;
    double lowMhz = 0;
    double highMhz = 0;

    bool empty() const { return bins.isEmpty() && lowMhz == 0 && highMhz == 0; }
    bool valid() const
    {
        constexpr qsizetype kMaximumBins = 1 << 20;
        return !bins.isEmpty() && bins.size() % sizeof(float) == 0
            && bins.size() / sizeof(float) <= kMaximumBins
            && std::isfinite(lowMhz) && std::isfinite(highMhz)
            && lowMhz >= 0 && highMhz > lowMhz
            && highMhz <= SharedCapturePolicy::kMaxMagnitudeHz / 1e6;
    }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::SpectrumCoverage)
