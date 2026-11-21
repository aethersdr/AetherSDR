#pragma once

#include "core/backends/RadioCapabilities.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <cmath>

namespace AetherSDR {

inline QString declaredBandButtonLabel(const QString& canonicalName,
                                       const QVector<DeclaredBandRange>& ranges)
{
    for (const DeclaredBandRange& band : ranges) {
        if (band.name.compare(canonicalName, Qt::CaseInsensitive) != 0
            || band.lowHz <= 0.0) {
            continue;
        }
        const double lowMhz = band.lowHz / 1.0e6;
        const double rounded = std::round(lowMhz);
        if (std::abs(lowMhz - rounded) < 1.0e-9) {
            return QString::number(static_cast<qint64>(rounded));
        }
    }
    return canonicalName;
}

inline bool declaredBandUtilityTargetAvailable(double targetMhz,
                                               double tuningMinMhz,
                                               double tuningMaxMhz)
{
    // The established range contract treats max <= min (including 0/0) as
    // "not reported", hence unconstrained. Gate only on affirmative evidence.
    return tuningMaxMhz <= tuningMinMhz
        || (targetMhz >= tuningMinMhz && targetMhz <= tuningMaxMhz);
}

// A radio that declares its own band set never shows radio-held (Flex) XVTR
// bands, but a client-side transverter configured in AetherSDR for that radio
// (networked Icom) is the operator's own band and always gets its button.
inline int configuredXvtrBandCount(bool radioDeclaredBandSet, int configuredCount,
                                   bool clientOwned = false)
{
    return radioDeclaredBandSet && !clientOwned ? 0 : configuredCount;
}

inline bool declaredBandMenuIncludesUtility(bool radioDeclaredBandSet,
                                            bool xvtrSetup,
                                            double targetMhz,
                                            double tuningMinMhz,
                                            double tuningMaxMhz)
{
    if (!radioDeclaredBandSet) {
        return true;
    }
    return !xvtrSetup && declaredBandUtilityTargetAvailable(
        targetMhz, tuningMinMhz, tuningMaxMhz);
}

} // namespace AetherSDR
