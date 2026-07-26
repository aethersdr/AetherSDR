#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <optional>

namespace AetherSDR {

struct FrequencyFrame {
    double centerMhz{0.0};
    double bandwidthMhz{0.0};

    bool isValid() const
    {
        return std::isfinite(centerMhz)
            && std::isfinite(bandwidthMhz)
            && centerMhz > 0.0
            && bandwidthMhz > 0.0;
    }
};

struct FrequencyPreviewTransform {
    double scale{1.0};
    double offset{0.0};
    bool valid{false};
};

inline double frequencyCanvasFraction(double localX, int contentWidth)
{
    const int safeWidth = std::max(1, contentWidth);
    return std::clamp(localX / static_cast<double>(safeWidth) - 0.5,
                      -0.5, 0.5);
}

inline double frequencyAtFraction(const FrequencyFrame& frame, double fraction)
{
    return frame.centerMhz + fraction * frame.bandwidthMhz;
}

inline double centerForAnchoredBandwidth(double anchorMhz,
                                         double anchorFraction,
                                         double bandwidthMhz)
{
    return anchorMhz - anchorFraction * bandwidthMhz;
}

inline FrequencyPreviewTransform frequencyPreviewTransform(
    const FrequencyFrame& base,
    const FrequencyFrame& target)
{
    if (!base.isValid() || !target.isValid()) {
        return {};
    }
    return FrequencyPreviewTransform{
        target.bandwidthMhz / base.bandwidthMhz,
        (target.centerMhz - base.centerMhz) / base.bandwidthMhz,
        true,
    };
}

inline double sourceUnitPosition(double targetUnitPosition,
                                 const FrequencyFrame& source,
                                 const FrequencyFrame& target)
{
    if (!source.isValid() || !target.isValid()
        || !std::isfinite(targetUnitPosition)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double targetFrequencyMhz = target.centerMhz
        + (targetUnitPosition - 0.5) * target.bandwidthMhz;
    return 0.5 + (targetFrequencyMhz - source.centerMhz)
        / source.bandwidthMhz;
}

struct FrequencyRangeCommand {
    double centerMhz{0.0};
    double bandwidthMhz{0.0};

    bool isValid() const
    {
        return FrequencyFrame{centerMhz, bandwidthMhz}.isValid();
    }
};

class FrequencyRangeCommandThrottle {
public:
    std::optional<FrequencyRangeCommand> request(
        const FrequencyRangeCommand& command,
        bool due,
        bool force)
    {
        if (!command.isValid()) {
            return std::nullopt;
        }
        m_pending = command;
        if (!due && !force) {
            return std::nullopt;
        }
        return takePending();
    }

    std::optional<FrequencyRangeCommand> takePending()
    {
        if (!m_pending.has_value()) {
            return std::nullopt;
        }
        const FrequencyRangeCommand command = *m_pending;
        m_pending.reset();
        return command;
    }

    void clear() { m_pending.reset(); }
    bool hasPending() const { return m_pending.has_value(); }

private:
    std::optional<FrequencyRangeCommand> m_pending;
};

enum class WaterfallPipelineMode {
    Legacy,
    RowFrequencyFrames,
};

struct WaterfallRowFrameReadiness {
    bool requested{false};
    bool formatSupported{false};
    bool textureCreated{false};
    bool samplerCreated{false};
    bool bindingsCreated{false};
    bool pipelineCreated{false};
};

inline WaterfallPipelineMode chooseWaterfallPipeline(
    const WaterfallRowFrameReadiness& readiness)
{
    return readiness.requested
            && readiness.formatSupported
            && readiness.textureCreated
            && readiness.samplerCreated
            && readiness.bindingsCreated
            && readiness.pipelineCreated
        ? WaterfallPipelineMode::RowFrequencyFrames
        : WaterfallPipelineMode::Legacy;
}

inline float waterfallScrollProgressRows(std::int64_t elapsedMs, float msPerRow,
                                         float distanceRows = 1.0f)
{
    if (elapsedMs <= 0 || !std::isfinite(msPerRow) || msPerRow <= 0.0f
        || !std::isfinite(distanceRows) || distanceRows <= 0.0f) {
        return 0.0f;
    }
    return std::clamp(static_cast<float>(elapsedMs) / msPerRow,
                      0.0f, distanceRows);
}

inline float waterfallScrollSampleOffsetUnit(float progressRows,
                                              float distanceRows,
                                              int textureRows)
{
    if (!std::isfinite(progressRows) || !std::isfinite(distanceRows)
        || distanceRows <= 0.0f || textureRows <= 0) {
        return 0.0f;
    }
    return (distanceRows
            - std::clamp(progressRows, 0.0f, distanceRows))
        / static_cast<float>(textureRows);
}

inline bool dssFftScaleSettleActive(std::int64_t nowMs,
                                    std::int64_t settleUntilMs)
{
    return settleUntilMs > 0 && nowMs < settleUntilMs;
}

inline float dssHistoryAvailability(float sampleAge, float validRows)
{
    if (!std::isfinite(sampleAge) || !std::isfinite(validRows)
        || validRows <= 0.0f) {
        return 0.0f;
    }
    return std::clamp(validRows - sampleAge, 0.0f, 1.0f);
}

// Mirror of dss_mesh.vert sampleHistoryDbm()'s retained sample-age clamp, for
// unit testing. It must track the shader exactly, including the two clamps the
// shader applies: cap validRows to [0, rows] (cappedValidRows) and floor the
// oldest retained age at 0. Omitting them makes the mirror diverge from the GPU
// path it validates outside 1 <= validRows <= rows (e.g. a negative age at
// validRows = 0.5, or an uncapped age when validRows > rows). std::nullopt is
// the analogue of the shader returning floorDbm (sample past the valid range).
inline std::optional<float> dssRetainedSampleAge(float sourceAge,
                                                 float remainingRows,
                                                 float validRows,
                                                 float rows)
{
    if (!std::isfinite(sourceAge) || !std::isfinite(remainingRows)
        || !std::isfinite(validRows) || !std::isfinite(rows)
        || sourceAge < 0.0f || remainingRows < 0.0f || rows < 1.0f) {
        return std::nullopt;
    }
    const float cappedValidRows = std::clamp(validRows, 0.0f, rows);
    if (sourceAge >= cappedValidRows) {
        return std::nullopt;
    }
    const float oldestRetainedAge = std::max(cappedValidRows - 1.0f, 0.0f);
    return std::min(sourceAge + remainingRows, oldestRetainedAge);
}

struct StablePresentationAnchor {
    float value{0.0f};
    float acquisitionMean{0.0f};
    int sampleCount{0};
    bool hasValue{false};
    bool locked{false};
};

inline bool observeStablePresentationAnchor(
    StablePresentationAnchor& anchor,
    float sample,
    int samplesToLock = 3,
    float quantum = 1.0f)
{
    if (!std::isfinite(sample) || samplesToLock <= 0
        || !std::isfinite(quantum) || quantum <= 0.0f) {
        return false;
    }
    if (anchor.locked) {
        return false;
    }

    if (!anchor.hasValue) {
        anchor.value = sample;
        anchor.acquisitionMean = sample;
        anchor.sampleCount = 1;
        anchor.hasValue = true;
    } else {
        anchor.sampleCount = std::min(anchor.sampleCount + 1, samplesToLock);
        anchor.acquisitionMean +=
            (sample - anchor.acquisitionMean)
            / static_cast<float>(anchor.sampleCount);
    }

    if (anchor.sampleCount >= samplesToLock) {
        anchor.value =
            std::round(anchor.acquisitionMean / quantum) * quantum;
        anchor.locked = true;
    }
    return true;
}

inline float stablePresentationValue(const StablePresentationAnchor& anchor,
                                     float fallback)
{
    if (anchor.hasValue && std::isfinite(anchor.value)) {
        return anchor.value;
    }
    return std::isfinite(fallback) ? fallback : 0.0f;
}

struct ObservedWaterfallCadence {
    std::int64_t lastRowTimestampMs{0};
    float msPerRow{100.0f};
    int sampleCount{0};
    bool valid{false};
};

inline bool observeWaterfallCadence(ObservedWaterfallCadence& cadence,
                                    std::int64_t rowTimestampMs)
{
    constexpr std::int64_t kMinIntervalMs = 10;
    constexpr std::int64_t kMaxIntervalMs = 15000;
    if (rowTimestampMs <= 0) {
        return false;
    }
    if (cadence.lastRowTimestampMs <= 0) {
        cadence.lastRowTimestampMs = rowTimestampMs;
        return false;
    }

    const std::int64_t intervalMs =
        rowTimestampMs - cadence.lastRowTimestampMs;
    if (intervalMs <= 0) {
        return false;
    }
    cadence.lastRowTimestampMs = rowTimestampMs;
    if (intervalMs < kMinIntervalMs || intervalMs > kMaxIntervalMs) {
        return false;
    }

    const float measuredMs = static_cast<float>(intervalMs);
    if (!cadence.valid) {
        cadence.msPerRow = measuredMs;
        cadence.sampleCount = 1;
        cadence.valid = true;
        return true;
    }

    // Track real arrival cadence while damping network jitter. Rate controls
    // explicitly reset the tracker, so the first interval at a new rate locks
    // immediately instead of being averaged against the previous setting.
    const float boundedMeasurement = std::clamp(
        measuredMs, cadence.msPerRow * 0.25f, cadence.msPerRow * 4.0f);
    const float alpha = cadence.sampleCount < 3 ? 0.5f : 0.2f;
    cadence.msPerRow += alpha * (boundedMeasurement - cadence.msPerRow);
    cadence.sampleCount = std::min(cadence.sampleCount + 1, 1000);
    return true;
}

inline bool observeWaterfallCadenceAndTimeScale(
    ObservedWaterfallCadence& cadence,
    StablePresentationAnchor& timeScaleAnchor,
    std::int64_t rowTimestampMs)
{
    if (!observeWaterfallCadence(cadence, rowTimestampMs)) {
        return false;
    }
    observeStablePresentationAnchor(timeScaleAnchor, cadence.msPerRow);
    return true;
}

inline float observedWaterfallMsPerRow(
    const ObservedWaterfallCadence& cadence,
    float fallbackMsPerRow)
{
    if (cadence.valid && std::isfinite(cadence.msPerRow)
        && cadence.msPerRow > 0.0f) {
        return cadence.msPerRow;
    }
    return std::isfinite(fallbackMsPerRow) && fallbackMsPerRow > 0.0f
        ? fallbackMsPerRow
        : 100.0f;
}

inline float selectedWaterfallMsPerRow(
    bool kiwiVisible,
    float flexMsPerRow,
    const ObservedWaterfallCadence* kiwiCadence,
    float kiwiFallbackMsPerRow = 100.0f)
{
    if (!kiwiVisible) {
        return std::isfinite(flexMsPerRow) && flexMsPerRow > 0.0f
            ? flexMsPerRow
            : 100.0f;
    }
    return kiwiCadence
        ? observedWaterfallMsPerRow(*kiwiCadence, kiwiFallbackMsPerRow)
        : observedWaterfallMsPerRow(
              ObservedWaterfallCadence{}, kiwiFallbackMsPerRow);
}

inline float selectedWaterfallTimeScaleMsPerRow(
    bool kiwiVisible,
    float flexMsPerRow,
    const StablePresentationAnchor* kiwiTimeScaleAnchor,
    float kiwiFallbackMsPerRow = 100.0f)
{
    if (!kiwiVisible) {
        return std::isfinite(flexMsPerRow) && flexMsPerRow > 0.0f
            ? flexMsPerRow
            : 100.0f;
    }
    if (!kiwiTimeScaleAnchor) {
        return std::isfinite(kiwiFallbackMsPerRow)
                && kiwiFallbackMsPerRow > 0.0f
            ? kiwiFallbackMsPerRow
            : 100.0f;
    }
    return stablePresentationValue(
        *kiwiTimeScaleAnchor, kiwiFallbackMsPerRow);
}

} // namespace AetherSDR
