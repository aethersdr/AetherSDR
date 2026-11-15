#pragma once

#include "core/DbmRangePlausibility.h"
#include "core/SpectrumDecodeScale.h"

#include <QtGlobal>
#include <QVector>
#include <QVarLengthArray>

#include <algorithm>
#include <cmath>
#include <optional>
#include <limits>

namespace AetherSDR::DbmRangeTransition {

struct Range {
    float minDbm{0.0f};
    float maxDbm{0.0f};
};

template<typename Dispatch, typename Commit>
bool dispatchValidatedRange(const Range& range, Dispatch dispatch, Commit commit)
{
    if (!dbmRangeLooksPlausible(range.minDbm, range.maxDbm) || !dispatch()) {
        return false;
    }
    commit();
    return true;
}

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

    HandshakeDecision completeReply(quint64 expectedGeneration, bool accepted,
                                     const Range& previousRange)
    {
        if (!m_active || m_generation != expectedGeneration) {
            return {};
        }
        // Principle II: a radio status received after the write is the truth,
        // accepted or not, as in FlexLib (an accepted reply changes nothing).
        // With no intervening status an accepted write stands; a rejected one
        // falls back to the prior range.
        const Range range = m_authoritativeRange.value_or(
            accepted ? m_requestedRange : previousRange);
        clear();
        return {HandshakeAction::ReconcileRadioRange, range};
    }

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

inline Range clippedFloorRecoveryRange(float currentMinDbm,
                                       float currentMaxDbm,
                                       float headroomStepDb = 6.0f)
{
    if (!std::isfinite(headroomStepDb) || headroomStepDb <= 0.0f) {
        return {currentMinDbm, currentMaxDbm};
    }
    // Keep the opposite endpoint: floor recovery must not clip existing peaks.
    // Flex's lower endpoint is -180 dBm; the supported aperture is <=180 dB.
    const float minDbm = std::min(currentMinDbm, std::max(
        {currentMinDbm - headroomStepDb, -180.0f, currentMaxDbm - 180.0f}));
    return {minDbm, currentMaxDbm};
}

inline Range clippedPeakRecoveryRange(float currentMinDbm,
                                      float currentMaxDbm,
                                      float headroomStepDb = 24.0f)
{
    if (!std::isfinite(headroomStepDb) || headroomStepDb <= 0.0f) {
        return {currentMinDbm, currentMaxDbm};
    }
    // FlexLib Panadapter.HighDbm caps the radio encoder ceiling at +20 dBm.
    const float maxDbm = std::max(currentMaxDbm, std::min(
        {currentMaxDbm + headroomStepDb, 20.0f, currentMinDbm + 180.0f}));
    return {currentMinDbm, maxDbm};
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

// A queued observation can have been decoded before a request, and several
// requested apertures can coexist on the wire during rapid reversals. Keep the
// bounded candidate history until expiry even after the target is observed.
class FrameGuard
{
public:
    void arm(Range oldRange, Range target, qint64 nowMs, qint64 timeoutMs)
    {
        if (nowMs > m_untilMs) {
            clear();
        }
        append(oldRange);
        const QVector<Range> inFlight = m_candidates;
        // Min and max can take effect on different FFTs. Preserve intermediate
        // endpoint combinations as well as complete requested apertures.
        for (const Range candidate : inFlight) {
            append({candidate.minDbm, target.maxDbm});
            append({target.minDbm, candidate.maxDbm});
        }
        append(target);
        m_target = target;
        m_targetGeneration = 0;
        m_untilMs = nowMs + timeoutMs;
    }

    void setTargetScale(const SpectrumDecodeScale& scale)
    {
        if (!materiallyDifferent(m_target, {scale.minDbm, scale.maxDbm}, 0.01f)) {
            m_targetGeneration = scale.generation;
        }
    }

    void clear()
    {
        m_candidates.clear();
        m_untilMs = 0;
        m_targetGeneration = 0;
    }

    Evaluation evaluate(const QVector<float>& bins, const QVector<float>& previous,
                        const SpectrumDecodeScale& decoded, qint64 nowMs) const
    {
        Evaluation result;
        if (nowMs > m_untilMs || !decoded.valid() || bins.isEmpty()
            || bins.size() != previous.size() || m_candidates.isEmpty()) {
            return result;
        }
        const Range decodeRange{decoded.minDbm, decoded.maxDbm};
        const float decodeSpan = decoded.maxDbm - decoded.minDbm;
        const qsizetype step = std::max<qsizetype>(1, bins.size() / 256);
        const auto errorFor = [&](Range wireRange) {
            QVarLengthArray<float, 256> errors;
            const float span = wireRange.maxDbm - wireRange.minDbm;
            for (qsizetype i = 0; i < bins.size(); i += step) {
                const float fraction = (decoded.maxDbm - bins[i]) / decodeSpan;
                const float value = wireRange.maxDbm - fraction * span;
                if (std::isfinite(value) && std::isfinite(previous[i])) {
                    errors.append(std::abs(value - previous[i]));
                }
            }
            if (errors.isEmpty()) {
                return std::numeric_limits<float>::infinity();
            }
            auto middle = errors.begin() + errors.size() / 2;
            std::nth_element(errors.begin(), middle, errors.end());
            return *middle;
        };
        constexpr float kImprovementDb = 0.75f;
        Range selected = decodeRange;
        float bestError = errorFor(selected);
        for (const Range candidate : m_candidates) {
            const float error = errorFor(candidate);
            if (error + kImprovementDb < bestError) {
                selected = candidate;
                bestError = error;
            }
        }
        if (materiallyDifferent(selected, decodeRange, 0.01f)) {
            result.useRebasedBins = true;
            result.rebasedBins = bins;
            const float span = selected.maxDbm - selected.minDbm;
            for (float& bin : result.rebasedBins) {
                const float fraction = (decoded.maxDbm - bin) / decodeSpan;
                bin = selected.maxDbm - fraction * span;
            }
        }
        // Matching values from an older decode generation cannot complete a
        // newer request, including an A→B→A reversal.
        result.newEncodingObserved = m_targetGeneration != 0
            && decoded.generation == m_targetGeneration
            && !materiallyDifferent(selected, m_target, 0.01f);
        return result;
    }

private:
    void append(Range range)
    {
        if (!dbmRangeLooksPlausible(range.minDbm, range.maxDbm)) {
            return;
        }
        for (const Range candidate : m_candidates) {
            if (!materiallyDifferent(candidate, range, 0.01f)) {
                return;
            }
        }
        constexpr qsizetype kMaximumCandidates = 32;
        if (m_candidates.size() == kMaximumCandidates) {
            m_candidates.removeAt(1); // Retain the original wire aperture.
        }
        m_candidates.append(range);
    }

    QVector<Range> m_candidates;
    Range m_target;
    quint64 m_targetGeneration{0};
    qint64 m_untilMs{0};
};

} // namespace AetherSDR::DbmRangeTransition
