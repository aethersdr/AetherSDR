#include "TxLinearityTelemetry.h"

#include "models/MeterModel.h"
#include <QDateTime>
#include <cmath>

namespace AetherSDR::txlin {

TxLinearityTelemetry::TxLinearityTelemetry(const MeterModel* meters)
    : m_meters(meters)
{
}

TelemetrySample TxLinearityTelemetry::observe(std::size_t index, double value,
                                              std::int64_t sourceStamp,
                                              std::int64_t wallNowMs,
                                              std::int64_t monotonicNowMs)
{
    Observation& previous = m_observations[index];
    if (sourceStamp <= 0 || !std::isfinite(value) || monotonicNowMs < 0) {
        previous = {};
        return {};
    }
    if (sourceStamp == previous.sourceStamp) {
        return previous.sample;
    }
    if (sourceStamp > wallNowMs || wallNowMs - sourceStamp > monotonicNowMs) {
        previous = {};
        return {};
    }
    previous.sourceStamp = sourceStamp;
    previous.sample = {value, monotonicNowMs - (wallNowMs - sourceStamp)};
    return previous.sample;
}

TxTelemetry TxLinearityTelemetry::read(std::int64_t monotonicNowMs)
{
    if (!m_meters) {
        m_observations = {};
        return {};
    }
    const std::int64_t wallNowMs = QDateTime::currentMSecsSinceEpoch();
    return {
        observe(0, m_meters->swr(), m_meters->swrSampleUpdatedAtMs(), wallNowMs, monotonicNowMs),
        observe(1, m_meters->paTemp(), m_meters->paTempUpdatedAtMs(), wallNowMs, monotonicNowMs),
        observe(2, m_meters->supplyVolts(), m_meters->supplyVoltsUpdatedAtMs(), wallNowMs, monotonicNowMs),
        observe(3, m_meters->fwdPowerInstant(), m_meters->fwdPowerUpdatedAtMs(), wallNowMs, monotonicNowMs)
    };
}

} // namespace AetherSDR::txlin
