#pragma once

#include <chrono>
#include <QString>
#include "core/TxCoordinator.h"

namespace AetherSDR {

// An admitted desktop edge, not a TX grant or a daemon protocol method.
struct CwControlRequest {
    enum class Kind { Carrier, PttEnvelope };
    Kind kind{Kind::Carrier};
    bool down{false};
    bool breakIn{false};
    int breakInDelayMs{0};
    std::chrono::steady_clock::time_point scheduledAt{};
    QString debugSource;
    quint64 debugTraceId{0};
    quint64 debugSourceMs{0};
    // A scheduled key-down's own key-up instant, when its producer knows it at
    // the start of the element (the iambic keyer does). On a key-up, the
    // earlier instant a keyer stop cut the element to; `scheduledAt` is then
    // still the element's deadline. Epoch zero: none. Only a backend that
    // places edges itself reads it (HL2, #5065): it schedules both edges of
    // an element at once, so the element's length does not depend on the
    // key-up's own hop, and keys up at a stop instead of at the deadline.
    std::chrono::steady_clock::time_point releaseAt{};
};


// The admission every CW control applies before a backend writes an edge: the
// edge matches its command, the operation is not independent, a key-down needs
// a transmit-capable radio, and the command still permits dispatch.
inline bool cwEdgeAdmitted(const CwControlRequest& request, const TxCoordinator::Command& command,
                           bool canTransmit)
{
    return request.down == command.keying && !command.operation.independent()
        && (!request.down || canTransmit)
        && command.permitsDispatch(TxCoordinator::monotonicMs());
}
} // namespace AetherSDR
