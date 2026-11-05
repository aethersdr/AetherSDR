#pragma once

#include "SliceDelta.h"

#include <atomic>
#include <memory>
#include <optional>

namespace AetherSDR {

// The transport revokes its epoch before disconnect/reconnect or destruction.
// Copies already queued to the owner thread retain the original epoch.
struct SliceObservation {
    int sliceId{-1};
    SliceDelta values;
    std::optional<quint32> ownerHandle;
    std::optional<QString> reportedModeList;
    std::optional<int> internalPaPower;
    std::shared_ptr<const std::atomic_bool> transportEpoch;

    bool current() const
    {
        return sliceId >= 0 && transportEpoch
            && transportEpoch->load(std::memory_order_acquire);
    }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::SliceObservation)
