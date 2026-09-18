#pragma once

#include <QStringView>
#include <optional>

namespace AetherSDR {

// HL2 restores its per-band gain through RadioStateMemory. Its legacy display
// value has no band identity and must not be replayed as an operator change
// (#5400). Keep the existing replay rules for every other family.
template<typename SetGain>
int restoreLegacyRfGain(QStringView family, bool clientOwnsGain,
                        std::optional<int> savedGain, int currentGain,
                        SetGain setGain)
{
    if (family != u"hl2" && clientOwnsGain && savedGain.has_value()) {
        setGain(*savedGain);
        return *savedGain;
    }
    return currentGain;
}

} // namespace AetherSDR
