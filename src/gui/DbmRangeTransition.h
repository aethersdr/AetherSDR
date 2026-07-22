#pragma once

#include <QtGlobal>
#include <QVector>
#include <QVarLengthArray>

#include <algorithm>
#include <cmath>
#include <optional>

namespace AetherSDR::DbmRangeTransition {

struct Range {
    float minDbm{0.0f};
    float maxDbm{0.0f};
};

inline bool materiallyDifferent(const Range& lhs, const Range& rhs,
                                float thresholdDb = 0.05f)
{
    return std::abs(lhs.minDbm - rhs.minDbm) > thresholdDb
        || std::abs(lhs.maxDbm - rhs.maxDbm) > thresholdDb;
}

enum class HandshakeAction {
    Ignore,
    ApplyRadioRange,
    HoldRequestedRange,
    ReconcileRadioRange,
    RetireWithoutEcho,
};

struct HandshakeDecision {
    HandshakeAction action{HandshakeAction::Ignore};
    Range range;
};

class Handshake
{
public:
    quint64 arm(float minDbm, float maxDbm, qint64 requestedMs)
    {
        m_active = true;
        m_requestedRange = {minDbm, maxDbm};
        m_requestedMs = requestedMs;
        m_authoritativeRange.reset();
        return ++m_generation;
    }

    HandshakeDecision observeRadioRange(float minDbm, float maxDbm,
                                        qint64 nowMs, qint64 timeoutMs)
    {
        const Range radioRange{minDbm, maxDbm};
        if (!m_active) {
            return {HandshakeAction::ApplyRadioRange, radioRange};
        }

        if (rangesMatch(radioRange, m_requestedRange)) {
            clear();
            return {HandshakeAction::ApplyRadioRange, radioRange};
        }

        // Keep the latest radio-owned range. PanadapterModel has already
        // accepted this status, so a timeout must not discard the only signal
        // that can bring the FFT decoder and visible scale back into agreement.
        m_authoritativeRange = radioRange;
        if (m_requestedMs > 0 && nowMs - m_requestedMs > timeoutMs) {
            clear();
            return {HandshakeAction::ReconcileRadioRange, radioRange};
        }
        return {HandshakeAction::HoldRequestedRange, m_requestedRange};
    }

    HandshakeDecision finish(quint64 expectedGeneration)
    {
        if (!m_active || m_generation != expectedGeneration) {
            return {};
        }

        if (m_authoritativeRange.has_value()) {
            const Range radioRange = *m_authoritativeRange;
            clear();
            return {HandshakeAction::ReconcileRadioRange, radioRange};
        }

        clear();
        return {HandshakeAction::RetireWithoutEcho, m_requestedRange};
    }

    HandshakeDecision cancelForRadioAuthority(float minDbm, float maxDbm)
    {
        if (!m_active) {
            return {};
        }

        const Range radioRange{minDbm, maxDbm};
        clear();
        return {HandshakeAction::ReconcileRadioRange, radioRange};
    }

    bool active() const { return m_active; }

private:
    static bool rangesMatch(const Range& left, const Range& right)
    {
        return std::abs(left.minDbm - right.minDbm) < 0.01f
            && std::abs(left.maxDbm - right.maxDbm) < 0.01f;
    }

    void clear()
    {
        m_active = false;
        m_requestedMs = 0;
        m_authoritativeRange.reset();
        ++m_generation;
    }

    bool m_active{false};
    Range m_requestedRange;
    qint64 m_requestedMs{0};
    quint64 m_generation{0};
    std::optional<Range> m_authoritativeRange;
};

inline float displaySpanDb(float dynamicRangeDb)
{
    // The 3D scale follows the same 10 dB minimum as the normal dBm scale.
    // A higher floor here makes arrow/drag changes below that floor invisible:
    // the underlying range moves while the rendered axis remains pinned.
    return std::clamp(dynamicRangeDb, 10.0f, 120.0f);
}

inline float floorDepthForDrag(float startDepthDb,
                               int startY,
                               int currentY,
                               int dragHeight)
{
    const int safeHeight = std::max(1, dragHeight);
    const float deltaDb =
        (static_cast<float>(startY - currentY) / safeHeight) * 24.0f;
    return std::clamp(startDepthDb + deltaDb, 0.0f, 24.0f);
}

inline float floorDepthFromOffsetDb(float floorOffsetDb)
{
    return std::clamp(-floorOffsetDb, 0.0f, 24.0f);
}

inline Range manualRequestRange(float requestedMinDbm,
                                float requestedMaxDbm,
                                bool flex3dActive,
                                float dssFloorDbm,
                                float dynamicRangeDb)
{
    if (!flex3dActive || !std::isfinite(dssFloorDbm)
        || dssFloorDbm <= -500.0f) {
        return {requestedMinDbm, requestedMaxDbm};
    }

    // Normalize in the shared request helper so arrow and drag paths cannot
    // disagree with the span the 3D renderer actually displays.
    const float dssSpanDb = displaySpanDb(dynamicRangeDb);
    if (!std::isfinite(dssSpanDb) || dssSpanDb <= 0.0f) {
        return {requestedMinDbm, requestedMaxDbm};
    }

    // The 3D axis is floor-anchored and does not use the hidden 2D reference
    // level. Ask the radio for the range actually drawn on that axis so its FFT
    // encoder cannot clip every bin to an unrelated 2D endpoint.
    return {dssFloorDbm, dssFloorDbm + dssSpanDb};
}

struct Evaluation {
    bool useRebasedBins{false};
    bool newEncodingObserved{false};
    QVector<float> rebasedBins;
};

inline Evaluation evaluate(const QVector<float>& sourceBins,
                           const QVector<float>& previousBins,
                           float oldMinDbm,
                           float oldMaxDbm,
                           float newMinDbm,
                           float newMaxDbm,
                           float minImprovementDb = 0.75f,
                           int maxErrorSamples = 256)
{
    Evaluation result;
    const float oldRange = oldMaxDbm - oldMinDbm;
    const float newRange = newMaxDbm - newMinDbm;
    if (oldRange <= 0.0f || newRange <= 0.0f
        || sourceBins.isEmpty() || sourceBins.size() != previousBins.size()) {
        return result;
    }

    const qsizetype sampleLimit = std::max(1, maxErrorSamples);
    const qsizetype step = std::max<qsizetype>(1, sourceBins.size() / sampleLimit);
    const qsizetype sampleCount = (sourceBins.size() + step - 1) / step;
    QVarLengthArray<float, 256> directErrors;
    QVarLengthArray<float, 256> rebasedErrors;
    directErrors.reserve(sampleCount);
    rebasedErrors.reserve(sampleCount);

    for (qsizetype i = 0; i < sourceBins.size(); i += step) {
        const float directDbm = sourceBins[i];
        const float fraction = (newMaxDbm - directDbm) / newRange;
        const float rebasedDbm = oldMaxDbm - fraction * oldRange;
        directErrors.append(std::abs(directDbm - previousBins[i]));
        rebasedErrors.append(std::abs(rebasedDbm - previousBins[i]));
    }

    auto median = [](QVarLengthArray<float, 256>& errors) {
        auto middle = errors.begin() + errors.size() / 2;
        std::nth_element(errors.begin(), middle, errors.end());
        return *middle;
    };
    const float directMedian = median(directErrors);
    const float rebasedMedian = median(rebasedErrors);
    if (rebasedMedian + minImprovementDb < directMedian) {
        result.useRebasedBins = true;
        result.rebasedBins = sourceBins;
        for (float& bin : result.rebasedBins) {
            const float fraction = (newMaxDbm - bin) / newRange;
            bin = oldMaxDbm - fraction * oldRange;
        }
    } else if (directMedian + minImprovementDb < rebasedMedian) {
        result.newEncodingObserved = true;
    }
    return result;
}

} // namespace AetherSDR::DbmRangeTransition
