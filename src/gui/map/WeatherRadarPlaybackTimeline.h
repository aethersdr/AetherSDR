#pragma once

#include <QDateTime>
#include <QSet>
#include <QVector>
#include <QtTypes>

#include <algorithm>
#include <limits>
#include <optional>

namespace AetherSDR {

struct WeatherRadarPlaybackPosition {
    int fromIndex{0};
    int toIndex{0};
    double progress{0.0};
    bool valid{false};
};

// One monotonic movie clock, with at most one presentation in flight. Normal
// elapsed time is independent of timer/paint acknowledgement ordering: retain
// the latest target time, not one 16 ms increment per completed paint. Capping
// that increment made rendering cost change the speed of the weather itself.
// Explicit discontinuities (suspension, missing data, lost acknowledgement)
// rebase the clock instead of accumulating an unbounded catch-up burst.
class WeatherRadarPlaybackCadence final {
public:
    void reset(qint64 elapsedMs = 0, qint64 wallTimeMs = 0)
    {
        m_presentedElapsedMs = std::max<qint64>(0, elapsedMs);
        m_pendingElapsedMs = m_presentedElapsedMs;
        m_lastTickMs = std::max<qint64>(0, wallTimeMs);
        m_targetElapsedMs = m_presentedElapsedMs;
        m_pendingSinceWallTimeMs = -1;
        // Keep the counter monotonic so a queued acknowledgement from the
        // renderer cannot match the first presentation after a reset.
        m_pendingPresentationSequence = 0;
    }

    void rebaseElapsed(qint64 elapsedMs)
    {
        m_presentedElapsedMs = std::max<qint64>(0, elapsedMs);
        m_pendingElapsedMs = m_presentedElapsedMs;
        m_targetElapsedMs = m_presentedElapsedMs;
        m_pendingSinceWallTimeMs = -1;
        m_pendingPresentationSequence = 0;
    }

    std::optional<qint64> timerTick(qint64 wallTimeMs,
                                    qint64 suspensionThresholdMs,
                                    qint64 presentationQuantumMs,
                                    qint64 presentationTimeoutMs =
                                        std::numeric_limits<qint64>::max())
    {
        if (wallTimeMs < 0 || suspensionThresholdMs <= 0
            || presentationQuantumMs <= 0 || presentationTimeoutMs <= 0) {
            return std::nullopt;
        }
        const qint64 wallDeltaMs = wallTimeMs >= m_lastTickMs
            ? wallTimeMs - m_lastTickMs : 0;
        m_lastTickMs = std::max(m_lastTickMs, wallTimeMs);
        if (wallDeltaMs >= suspensionThresholdMs) {
            // A suspended/blocked UI has not displayed the intervening movie.
            // Resume one presentation step ahead, without skipping the loop.
            m_targetElapsedMs = m_pendingPresentationSequence != 0
                ? m_pendingElapsedMs : m_presentedElapsedMs;
        }
        const qint64 advanceMs = wallDeltaMs >= suspensionThresholdMs
            ? presentationQuantumMs : wallDeltaMs;
        m_targetElapsedMs += std::min(advanceMs,
            std::numeric_limits<qint64>::max() - m_targetElapsedMs);
        if (m_pendingPresentationSequence != 0) {
            if (m_pendingSinceWallTimeMs >= 0
                && wallTimeMs >= m_pendingSinceWallTimeMs
                && wallTimeMs - m_pendingSinceWallTimeMs
                    >= presentationTimeoutMs) {
                // Retire only the sequence token, then retry the exact elapsed
                // position. Both renderers retain a single pending token, so a
                // late acknowledgement is ignored and the retry overwrites the
                // unpainted request without ever admitting two in-flight
                // presentation states.
                const qint64 retryElapsedMs = m_pendingElapsedMs;
                m_pendingElapsedMs = m_presentedElapsedMs;
                m_targetElapsedMs = retryElapsedMs;
                m_pendingSinceWallTimeMs = -1;
                m_pendingPresentationSequence = 0;
                return retryElapsedMs;
            }
            return std::nullopt;
        }
        return m_targetElapsedMs;
    }

    quint64 beginPresentation(qint64 elapsedMs)
    {
        if (elapsedMs < 0 || m_pendingPresentationSequence != 0) {
            return 0;
        }
        ++m_presentationCounter;
        if (m_presentationCounter == 0) {
            ++m_presentationCounter;
        }
        m_pendingElapsedMs = elapsedMs;
        // The caller may clamp to an exact final-frame/loop boundary. Rebase
        // there so the full one-second hold begins with its presentation.
        m_targetElapsedMs = elapsedMs;
        m_pendingSinceWallTimeMs = m_lastTickMs;
        m_pendingPresentationSequence = m_presentationCounter;
        return m_pendingPresentationSequence;
    }

    void rejectPresentation(quint64 presentationSequence)
    {
        if (presentationSequence == 0
            || presentationSequence != m_pendingPresentationSequence) {
            return;
        }
        m_pendingElapsedMs = m_presentedElapsedMs;
        // A decode/upload rejection is explicit buffering, not render lag.
        // Retain the displayed image and discard only this unavailable span.
        m_targetElapsedMs = m_presentedElapsedMs;
        m_pendingSinceWallTimeMs = -1;
        m_pendingPresentationSequence = 0;
    }

    std::optional<qint64> completePresentation(
        quint64 presentationSequence)
    {
        if (presentationSequence == 0
            || presentationSequence != m_pendingPresentationSequence) {
            return std::nullopt;
        }
        m_presentedElapsedMs = m_pendingElapsedMs;
        m_pendingSinceWallTimeMs = -1;
        m_pendingPresentationSequence = 0;
        if (m_targetElapsedMs <= m_presentedElapsedMs) {
            return std::nullopt;
        }
        return m_targetElapsedMs;
    }

    qint64 presentedElapsedMs() const { return m_presentedElapsedMs; }
    qint64 requestedElapsedMs() const
    {
        return presentationPending() ? m_pendingElapsedMs : m_presentedElapsedMs;
    }
    bool presentationPending() const
    {
        return m_pendingPresentationSequence != 0;
    }

private:
    qint64 m_presentedElapsedMs{0};
    qint64 m_pendingElapsedMs{0};
    qint64 m_lastTickMs{0};
    qint64 m_targetElapsedMs{0};
    qint64 m_pendingSinceWallTimeMs{-1};
    quint64 m_presentationCounter{0};
    quint64 m_pendingPresentationSequence{0};
};

inline QSet<int> weatherRadarPlaybackDecodedWindow(
    int fromIndex, int frameCount, int lookahead)
{
    QSet<int> indexes;
    if (fromIndex < 0 || fromIndex >= frameCount || frameCount < 2
        || lookahead < 2) {
        return indexes;
    }
    // Keep the displayed observation, its predecessor, and forward lookahead.
    // A pending upload must not evict the original still on screen.
    for (int offset = -1; offset < lookahead; ++offset) {
        indexes.insert((fromIndex + offset + frameCount) % frameCount);
    }
    return indexes;
}

inline QVector<int> weatherRadarPlaybackSegmentDurations(
    const QVector<QDateTime>& frames,
    qint64 observedMillisecondsPerPlaybackSecond,
    int minimumSegmentMs, int maximumSegmentMs)
{
    QVector<int> durations;
    if (frames.size() < 2 || observedMillisecondsPerPlaybackSecond <= 0
        || minimumSegmentMs <= 0 || maximumSegmentMs < minimumSegmentMs) {
        return durations;
    }
    QVector<qint64> gaps;
    gaps.reserve(frames.size() - 1);
    for (qsizetype index = 1; index < frames.size(); ++index) {
        const qint64 gap = frames.at(index - 1).msecsTo(frames.at(index));
        if (gap <= 0) {
            return {};
        }
        gaps.append(gap);
    }
    // Select ONE observed-to-display time scale for the WHOLE history. Applying
    // the min/max independently to each gap destroys that invariant: with the
    // 450 ms minimum, 106/132/358-second gaps became 450/450/597 ms. A constant-
    // velocity storm then appeared to accelerate, particularly across missing
    // scans, even though the phase clock itself was perfectly linear.
    const auto [shortest, longest] = std::minmax_element(gaps.cbegin(), gaps.cend());
    const double requestedScale = 1000.0 / observedMillisecondsPerPlaybackSecond;
    const double minimumScale = static_cast<double>(minimumSegmentMs) / *shortest;
    const double maximumScale = static_cast<double>(maximumSegmentMs) / *longest;
    // Both duration bounds are attainable only if the source-gap ratio fits
    // inside their ratio. Otherwise prioritize the maximum, allowing brief
    // intervals: one near-duplicate timestamp must not stretch the whole movie
    // into minutes. Never repair that conflict by giving different intervals
    // different speeds. Interior observations are knots, not required stops.
    const double scale = minimumScale <= maximumScale
        ? std::clamp(requestedScale, minimumScale, maximumScale)
        : std::min(requestedScale, maximumScale);
    durations.reserve(gaps.size());
    for (const qint64 gap : gaps) {
        // Millisecond quantization is the only per-interval adjustment. The
        // common maximumScale already bounds rounding within the int range.
        durations.append(std::max(1, qRound(static_cast<double>(gap) * scale)));
    }
    return durations;
}

// Samples the whole radar loop from one monotonic clock. Segment durations
// expand when one or two source observations are absent, keeping apparent
// storm velocity stable instead of jumping across the gap. The final frame is
// held and the clock then resets directly to the oldest frame; morphing the
// newest observation backward through an hour of weather is misleading.
inline WeatherRadarPlaybackPosition weatherRadarPlaybackPosition(
    qint64 elapsedMs, int frameCount, const QVector<int>& segmentDurationsMs,
    int loopPauseMs)
{
    WeatherRadarPlaybackPosition position;
    if (elapsedMs < 0 || frameCount < 2 || loopPauseMs < 0
        || segmentDurationsMs.size() < frameCount - 1) {
        return position;
    }
    qint64 forwardDuration = 0;
    for (int index = 0; index < frameCount - 1; ++index) {
        const int duration = segmentDurationsMs.at(index);
        if (duration <= 0) {
            return position;
        }
        forwardDuration += duration;
    }
    const qint64 cycleDuration = forwardDuration + loopPauseMs;
    if (cycleDuration <= 0) {
        return position;
    }
    qint64 phase = elapsedMs % cycleDuration;
    position.valid = true;
    position.fromIndex = frameCount - 1;
    position.toIndex = frameCount - 1;
    position.progress = 1.0;
    for (int index = 0; index < frameCount - 1; ++index) {
        const int duration = segmentDurationsMs.at(index);
        if (phase < duration) {
            position.fromIndex = static_cast<int>(index);
            position.toIndex = position.fromIndex + 1;
            position.progress = static_cast<double>(phase) / duration;
            break;
        }
        phase -= duration;
    }
    position.progress = std::clamp(position.progress, 0.0, 1.0);
    return position;
}

// Slider percentages map directly to 0.25x through 5x. Invalid persisted or
// external values use 1x; do not quantize valid fine-tuned speeds to presets.
inline int weatherRadarPlaybackSpeedPercent(int requested)
{
    return requested >= 25 && requested <= 500 ? requested : 100;
}

inline QVector<int> weatherRadarPlaybackScaledDurations(
    const QVector<int>& normalDurations, int speedPercent)
{
    QVector<int> result;
    const int speed = weatherRadarPlaybackSpeedPercent(speedPercent);
    for (const int duration : normalDurations) {
        if (duration <= 0) {
            return {};
        }
        result.append(static_cast<int>(std::clamp<qint64>(
            (static_cast<qint64>(duration) * 100 + speed / 2) / speed,
            1, std::numeric_limits<int>::max())));
    }
    return result;
}

// Preserve the same original and fraction of its dwell when speed changes.
// Final-frame hold time is wall time, not playback speed: changing speed during
// that hold must neither rewind the observation nor shorten/extend the hold.
inline qint64 weatherRadarPlaybackElapsedAtSpeed(
    qint64 elapsedMs, const QVector<int>& before, const QVector<int>& after,
    int loopPauseMs)
{
    if (elapsedMs < 0 || before.isEmpty() || before.size() != after.size()
        || loopPauseMs < 0) {
        return 0;
    }
    qint64 oldTotal = loopPauseMs;
    for (qsizetype index = 0; index < before.size(); ++index) {
        if (before.at(index) <= 0 || after.at(index) <= 0) {
            return 0;
        }
        oldTotal += before.at(index);
    }
    qint64 phase = elapsedMs % oldTotal;
    qint64 nextElapsed = 0;
    for (qsizetype index = 0; index < before.size(); ++index) {
        if (phase < before.at(index)) {
            return nextElapsed + phase * after.at(index) / before.at(index);
        }
        phase -= before.at(index);
        nextElapsed += after.at(index);
    }
    return nextElapsed + phase;
}

// Original-observation playback is deliberately a step function. There is no
// interpolation between NOAA scans: every pixel comes from one observation,
// including the final hold and the discontinuous newest-to-oldest loop reset.
inline WeatherRadarPlaybackPosition weatherRadarObservationPlaybackPosition(
    qint64 elapsedMs, int frameCount, const QVector<int>& segmentDurationsMs,
    int loopPauseMs)
{
    WeatherRadarPlaybackPosition position = weatherRadarPlaybackPosition(
        elapsedMs, frameCount, segmentDurationsMs, loopPauseMs);
    position.toIndex = position.fromIndex;
    position.progress = 0.0;
    return position;
}

// Unlike interpolated playback, do not skip an interior observation if a paint
// is delayed. Stop at the next frame boundary; if its image is still decoding
// or uploading, the caller retains the preceding original and retries.
inline qint64 weatherRadarObservationClampToBoundary(
    qint64 presentedElapsedMs, qint64 candidateElapsedMs,
    const QVector<int>& segmentDurationsMs, int loopPauseMs)
{
    if (presentedElapsedMs < 0 || candidateElapsedMs <= presentedElapsedMs
        || segmentDurationsMs.isEmpty() || loopPauseMs < 0) {
        return candidateElapsedMs;
    }
    qint64 cycleDuration = loopPauseMs;
    for (const int duration : segmentDurationsMs) {
        if (duration <= 0) {
            return candidateElapsedMs;
        }
        cycleDuration += duration;
    }
    qint64 boundary = (presentedElapsedMs / cycleDuration) * cycleDuration;
    for (const int duration : segmentDurationsMs) {
        boundary += duration;
        if (boundary > presentedElapsedMs) {
            return std::min(candidateElapsedMs, boundary);
        }
    }
    return std::min(candidateElapsedMs, boundary + loopPauseMs);
}

} // namespace AetherSDR
