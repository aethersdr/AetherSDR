#pragma once

#include <QStringView>
#include <optional>

namespace AetherSDR {

// HL2 restores per-band gain and ANAN restores per-ADC attenuation through
// RadioStateMemory. Their legacy display values lack the corresponding band
// or radio identity and must not be replayed as operator changes (#5400, #5820).
// Keep the existing replay rules for every other family.
template<typename SetGain>
int restoreLegacyRfGain(QStringView family, bool clientOwnsGain,
                        std::optional<int> savedGain, int currentGain,
                        SetGain setGain)
{
    if (family != u"hl2" && family != u"anan" && clientOwnsGain && savedGain.has_value()) {
        setGain(*savedGain);
        return *savedGain;
    }
    return currentGain;
}

} // namespace AetherSDR
