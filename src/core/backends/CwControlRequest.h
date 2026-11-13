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
