#pragma once

#include <algorithm>
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

} // namespace AetherSDR
