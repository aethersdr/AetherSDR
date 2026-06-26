#pragma once

#include <QVector>

#include <algorithm>
#include <cmath>

namespace AetherSDR::KiwiSdrTraceMath {

struct TraceFloorState {
    float floorDbm{-1000.0f};
    bool valid{false};
};

inline float interpolatedBinSample(const QVector<float>& bins,
                                   double srcCenter,
                                   float fallback)
{
    const int n = bins.size();
    if (n <= 0) {
        return fallback;
    }
    if (n == 1) {
        return std::isfinite(bins[0]) ? bins[0] : fallback;
    }

    const double clamped = std::clamp(srcCenter, 0.0, static_cast<double>(n - 1));
    const int left = std::clamp(static_cast<int>(std::floor(clamped)), 0, n - 1);
    const int right = std::min(left + 1, n - 1);
    const float leftValue = std::isfinite(bins[left]) ? bins[left] : fallback;
    const float rightValue = std::isfinite(bins[right]) ? bins[right] : leftValue;
    const float frac = static_cast<float>(clamped - static_cast<double>(left));
    return leftValue + frac * (rightValue - leftValue);
}

inline float averagedBinSample(const QVector<float>& bins,
                               double srcLeft,
                               double srcRight,
                               double srcCenter,
                               float fallback)
{
    const int n = bins.size();
    if (n <= 0 || srcRight <= 0.0 || srcLeft >= static_cast<double>(n)) {
        return fallback;
    }

    const double clampedLeft = std::clamp(srcLeft, 0.0, static_cast<double>(n));
    const double clampedRight = std::clamp(srcRight, 0.0, static_cast<double>(n));
    if (clampedRight - clampedLeft <= 1.0) {
        return interpolatedBinSample(bins, srcCenter, fallback);
    }

    const int first = std::clamp(static_cast<int>(std::floor(clampedLeft)), 0, n - 1);
    const int last = std::clamp(static_cast<int>(std::ceil(clampedRight)) - 1, 0, n - 1);
    double weightedSum = 0.0;
    double totalWeight = 0.0;
    for (int i = first; i <= last; ++i) {
        const float value = bins[i];
        if (!std::isfinite(value)) {
            continue;
        }

        const double binLeft = static_cast<double>(i);
        const double binRight = binLeft + 1.0;
        const double weight =
            std::max(0.0, std::min(clampedRight, binRight)
                - std::max(clampedLeft, binLeft));
        if (weight <= 0.0) {
            continue;
        }
        weightedSum += static_cast<double>(value) * weight;
        totalWeight += weight;
    }

    if (totalWeight <= 0.0) {
        return interpolatedBinSample(bins, srcCenter, fallback);
    }
    return static_cast<float>(weightedSum / totalWeight);
}

inline QVector<float> mapRowToTrace(const QVector<float>& rowBins,
                                    int destWidth,
                                    double rowCenterMhz,
                                    double rowBandwidthMhz,
                                    double viewCenterMhz,
                                    double viewBandwidthMhz,
                                    float fallback)
{
    if (rowBins.isEmpty() || destWidth <= 0
        || rowBandwidthMhz <= 0.0 || viewBandwidthMhz <= 0.0) {
        return {};
    }

    QVector<float> trace(destWidth, fallback);
    const double rowLowMhz = rowCenterMhz - rowBandwidthMhz * 0.5;
    const double viewLowMhz = viewCenterMhz - viewBandwidthMhz * 0.5;
    const int srcSize = rowBins.size();
    for (int x = 0; x < destWidth; ++x) {
        const double freqMhz = viewLowMhz
            + (static_cast<double>(x) + 0.5)
                * viewBandwidthMhz / static_cast<double>(destWidth);
        const double srcCenter =
            ((freqMhz - rowLowMhz) / rowBandwidthMhz)
                * static_cast<double>(srcSize) - 0.5;
        if (srcCenter < -0.5
            || srcCenter > static_cast<double>(srcSize) - 0.5) {
            continue;
        }

        const double srcLeft = std::max(0.0, srcCenter - 0.5);
        const double srcRight =
            std::min(static_cast<double>(srcSize), srcCenter + 0.5);
        trace[x] = averagedBinSample(rowBins, srcLeft, srcRight,
                                     srcCenter, fallback);
    }
    return trace;
}

inline float estimateTraceFloorDbm(const QVector<float>& bins,
                                   float minDbm,
                                   int maxSamples = 512)
{
    if (bins.isEmpty()) {
        return -1000.0f;
    }

    const int stride = std::max(1, static_cast<int>(bins.size() / maxSamples));
    const float blankThreshold = minDbm + 0.5f;
    float sum = 0.0f;
    int count = 0;
    for (int i = 0; i < bins.size(); i += stride) {
        const float value = bins[i];
        if (std::isfinite(value) && value > blankThreshold) {
            sum += value;
            ++count;
        }
    }
    if (count < 8) {
        return -1000.0f;
    }

    const float mean = sum / static_cast<float>(count);
    float floorSum = 0.0f;
    int floorCount = 0;
    for (int i = 0; i < bins.size(); i += stride) {
        const float value = bins[i];
        if (std::isfinite(value) && value > blankThreshold
            && value <= mean) {
            floorSum += value;
            ++floorCount;
        }
    }
    return (floorCount > 0)
        ? floorSum / static_cast<float>(floorCount)
        : mean;
}

inline void stabilizeTraceFloor(QVector<float>& bins,
                                TraceFloorState& state,
                                bool allowFloorAdapt,
                                float minDbm,
                                float maxDbm)
{
    const float frameFloor = estimateTraceFloorDbm(bins, minDbm);
    if (frameFloor <= -500.0f || !std::isfinite(frameFloor)) {
        return;
    }

    if (!state.valid
        || !std::isfinite(state.floorDbm)
        || state.floorDbm <= -500.0f) {
        state.floorDbm = frameFloor;
        state.valid = true;
    } else if (allowFloorAdapt) {
        const float delta = frameFloor - state.floorDbm;
        const float alpha = delta > 0.0f ? 0.03f : 0.12f;
        state.floorDbm += delta * alpha;
    }

    const float offset = state.floorDbm - frameFloor;
    if (std::abs(offset) < 0.05f) {
        return;
    }

    const float blankThreshold = minDbm + 0.5f;
    for (float& value : bins) {
        if (std::isfinite(value) && value > blankThreshold) {
            value = std::clamp(value + offset, minDbm, maxDbm);
        }
    }
}

} // namespace AetherSDR::KiwiSdrTraceMath
