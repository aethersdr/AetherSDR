#pragma once

#include "core/TxCoordinator.h"

namespace AetherSDR {

// Qualification is deliberately narrower than canTransmit. A backend must
// implement the matching stop verb and operation-bound transport evidence.
struct IndependentTxControl {
    unsigned activities{0};
};

// A queued certificate is invalidated at its source on contradictory input or
// disconnect, before its receiver can run. It is not a cached model TX state.
struct TxStopEvidence {
    TxCoordinator::StopRequest request;
    std::shared_ptr<const std::atomic<bool>> current;
    qint64 validUntilMs{0}; // zero only for exact-operation local no-dispatch evidence
    [[nodiscard]] bool valid(qint64 now = TxCoordinator::monotonicMs()) const
    {
        return current && current->load(std::memory_order_acquire) && request.valid()
            && now >= 0 && (validUntilMs == 0 || now < validUntilMs);
    }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::TxStopEvidence)
