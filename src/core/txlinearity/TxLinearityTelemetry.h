#pragma once

#include "TxLinearitySafety.h"

#include <QPointer>
#include <array>
#include <cstddef>

namespace AetherSDR {
class MeterModel;
}

namespace AetherSDR::txlin {

// One reader per original radio session, used on the model's owner thread.
// Received-sample timestamps are mapped once into the controller's monotonic
// clock. Polling cached values cannot renew them, even after a wall-clock step.
class TxLinearityTelemetry {
public:
    explicit TxLinearityTelemetry(const MeterModel* meters);
    TxTelemetry read(std::int64_t monotonicNowMs);

private:
    struct Observation {
        std::int64_t sourceStamp = 0;
        TelemetrySample sample;
    };
    TelemetrySample observe(std::size_t index, double value, std::int64_t sourceStamp,
                            std::int64_t wallNowMs, std::int64_t monotonicNowMs);

    QPointer<const MeterModel> m_meters;
    std::array<Observation, 4> m_observations;
};

} // namespace AetherSDR::txlin
