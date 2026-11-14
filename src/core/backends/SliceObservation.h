#pragma once

#include "SliceDelta.h"

#include <optional>

namespace AetherSDR {

// One Flex "slice N" report: its values, and the owner and extras the model
// needs for ownership. Stamped with the status session it arrived in.
struct SliceObservation {
    int sliceId{-1};
    SliceDelta values;
    std::optional<quint32> ownerHandle;
    std::optional<QString> reportedModeList;
    std::optional<int> internalPaPower;
    quint64 statusEpoch{0};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::SliceObservation)
