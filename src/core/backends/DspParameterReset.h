#pragma once

#include <QList>
#include <QMetaType>
#include <QStringList>

namespace AetherSDR {

// A parameter reset never changes an algorithm's enabled state.
enum class DspParameterResetTarget { All, Nr, Anf, Nrl, Anfl, Nrf, Nrs, Anft };

inline bool validDspParameterResetTarget(DspParameterResetTarget target)
{
    return target >= DspParameterResetTarget::All && target <= DspParameterResetTarget::Anft;
}

struct DspParameterResetControl {
    enum class AdvancedAccess { Unknown, Denied, Granted };
    QList<DspParameterResetTarget> hardwareTargets;
    AdvancedAccess advancedAccess{AdvancedAccess::Unknown};
    QStringList modes;
    QStringList notchModes;

    bool permits(DspParameterResetTarget target) const
    {
        return validDspParameterResetTarget(target) && hardwareTargets.contains(target)
            && (target == DspParameterResetTarget::All || target == DspParameterResetTarget::Nrs
                || advancedAccess == AdvancedAccess::Granted);
    }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::DspParameterResetTarget)
