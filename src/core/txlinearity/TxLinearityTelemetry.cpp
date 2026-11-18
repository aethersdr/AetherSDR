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
        // Several accepted samples can share a UTC millisecond. Keep the
        // original mapped age while mirroring the latest accepted value.
        previous.sample.value = value;
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

TelemetrySample TxLinearityTelemetry::observeReceived(std::size_t index, double value,
    std::int64_t receivedNs, std::int64_t receiptClockNowNs, std::int64_t monotonicNowMs)
{
    Observation& previous = m_observations[index];
    if (receivedNs <= 0 || receivedNs > receiptClockNowNs || !std::isfinite(value)
        || monotonicNowMs < 0 || (receiptClockNowNs - receivedNs) / 1000000 > monotonicNowMs) {
        previous = {};
        return {};
    }
    if (receivedNs != previous.sourceStamp) {
        previous.sourceStamp = receivedNs;
        previous.sample = {value, monotonicNowMs - (receiptClockNowNs - receivedNs) / 1000000};
    }
    return previous.sample;
}

TxTelemetry TxLinearityTelemetry::read(std::int64_t monotonicNowMs)
{
    TxTelemetry result;
    result.swrSource = TxTelemetry::SwrSource::DirectionalPower;
    if (!m_meters) {
        m_observations = {};
        return result;
    }
    const std::int64_t wallNowMs = QDateTime::currentMSecsSinceEpoch();
    result.radioSwr = observe(0, m_meters->swr(), m_meters->swrSampleUpdatedAtMs(), wallNowMs, monotonicNowMs);
    result.paTempC = observe(1, m_meters->paTemp(), m_meters->paTempUpdatedAtMs(), wallNowMs, monotonicNowMs);
    result.supplyVolts = observe(2, m_meters->supplyVolts(), m_meters->supplyVoltsUpdatedAtMs(), wallNowMs, monotonicNowMs);
    result.forwardWatts = observe(3, m_meters->fwdPowerInstant(), m_meters->fwdPowerUpdatedAtMs(), wallNowMs, monotonicNowMs);
    const std::optional<MeterModel::DirectionalPowerSample> pair = m_meters->directionalPowerSample();
    if (!pair) {
        for (std::size_t index = 4; index < m_observations.size(); ++index) { m_observations[index] = {}; }
        return result;
    }
    const std::int64_t clockNow = receiveMonotonicNs();
    result.directionalForwardWatts = observeReceived(4, pair->forwardWatts, pair->receipt.monotonicNs, clockNow, monotonicNowMs);
    result.directionalReflectedWatts = observeReceived(5, pair->reflectedWatts, pair->receipt.monotonicNs, clockNow, monotonicNowMs);
    result.forwardWatts = result.directionalForwardWatts;
    // No clamp or radio-SWR fallback: an impossible ratio is missing telemetry.
    if (pair->forwardWatts > 0 && pair->reflectedWatts >= 0 && pair->reflectedWatts < pair->forwardWatts) {
        const double rho = std::sqrt(double(pair->reflectedWatts) / pair->forwardWatts);
        result.swr = observeReceived(6, (1 + rho) / (1 - rho), pair->receipt.monotonicNs, clockNow, monotonicNowMs);
    } else {
        m_observations[6] = {};
    }
    return result;
}

} // namespace AetherSDR::txlin
