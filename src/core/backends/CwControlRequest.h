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

} // namespace AetherSDR
