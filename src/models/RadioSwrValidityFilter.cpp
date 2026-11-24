#include "RadioSwrValidityFilter.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

RadioSwrValidityFilter::Result RadioSwrValidityFilter::update(
    float forwardPowerInstant, float rawSwr,
    qint64 timestampMs, float maximumDisplayedSwr)
{
    const qint64 boundedTimestampMs = std::max<qint64>(timestampMs, 0);
    const qint64 effectiveTimestampMs =
        m_lastTimestampMs >= 0
            ? std::max(boundedTimestampMs, m_lastTimestampMs)
            : boundedTimestampMs;
    if (m_lastTimestampMs >= 0) {
        const double elapsedMs =
            static_cast<double>(effectiveTimestampMs - m_lastTimestampMs);
        const double release =
            std::exp2(-elapsedMs / kEnvelopeHalfLifeMs);
        m_forwardEnvelopeWatts =
            static_cast<float>(m_forwardEnvelopeWatts * release);
    }
    m_lastTimestampMs = effectiveTimestampMs;

    const bool finiteForward = std::isfinite(forwardPowerInstant);
    if (finiteForward && forwardPowerInstant > 0.0f) {
        m_forwardEnvelopeWatts =
            std::max(m_forwardEnvelopeWatts, forwardPowerInstant);
    }

    const float minimumForwardPower = std::max(
        kMinimumForwardPowerWatts,
        m_forwardEnvelopeWatts * kEnvelopeThresholdFraction);
    const bool hasAbsolutePower =
        finiteForward && forwardPowerInstant >= kMinimumForwardPowerWatts;
    const bool hasMeaningfulPower =
        hasAbsolutePower && forwardPowerInstant >= minimumForwardPower;
    bool held = false;

    if (!std::isfinite(rawSwr)) {
        held = m_hasReading;
        m_lowSwrCandidateSamples = 0;
        clearTimedConfirmations();
    } else if (!hasAbsolutePower) {
        // No measurable carrier means there is no trustworthy SWR sample.
        // In particular, do not let repeated no-carrier packets eventually
        // satisfy either of the time-based confirmation windows.
        held = m_hasReading;
        m_lowSwrCandidateSamples = 0;
        clearTimedConfirmations();
    } else if (rawSwr < 1.0f) {
        // SWR below 1 is physically impossible and may be the radio's
        // unavailable/over-range sentinel. A single corrupt sample must not
        // flash a frightening full-scale warning, but persistence under any
        // measurable carrier remains actionable even during power foldback.
        if (confirmationElapsed(m_belowUnityStartedAtMs,
                                effectiveTimestampMs)) {
            m_displayedSwr = maximumDisplayedSwr;
            m_hasReading = true;
            clearTimedConfirmations();
        } else {
            held = m_hasReading;
            m_lowPowerRecoveryStartedAtMs = -1;
        }
        m_lowSwrCandidateSamples = 0;
    } else if (!hasMeaningfulPower) {
        if (m_hasReading && rawSwr > m_displayedSwr) {
            // Preserve worsening foldback warnings only while measurable RF is
            // still present. Safety-significant rises remain immediate even
            // while forward power is below the relative envelope threshold.
            m_displayedSwr = rawSwr;
            m_lowSwrCandidateSamples = 0;
            clearTimedConfirmations();
        } else if (m_hasReading && rawSwr < m_displayedSwr) {
            // A real re-match or ALC recovery can improve SWR while power is
            // temporarily below the envelope threshold. Require one continuous,
            // bounded wall-clock confirmation for every physically valid
            // decrease, including near-unity readings, instead of holding the
            // stale warning until the 750 ms envelope has completely released.
            if (confirmationElapsed(m_lowPowerRecoveryStartedAtMs,
                                    effectiveTimestampMs)) {
                m_displayedSwr = rawSwr;
                m_hasReading = true;
                clearTimedConfirmations();
            } else {
                held = true;
                // rawSwr >= 1.0 here, so this sample breaks any run of
                // below-unity sentinels: reset that window (but keep the
                // recovery window advancing) so an interrupted sub-1.0 streak
                // cannot later peg the meter to full scale on a stale start.
                m_belowUnityStartedAtMs = -1;
            }
            m_lowSwrCandidateSamples = 0;
        } else {
            held = m_hasReading;
            m_lowSwrCandidateSamples = 0;
            clearTimedConfirmations();
        }
    } else if (rawSwr <= kLowSwrConfirmationMaximum
               && m_hasReading
               && m_displayedSwr > kLowSwrConfirmationMaximum) {
        clearTimedConfirmations();
        ++m_lowSwrCandidateSamples;
        if (m_lowSwrCandidateSamples >= kLowSwrConfirmationSamples) {
            m_displayedSwr = rawSwr;
            m_hasReading = true;
            m_lowSwrCandidateSamples = 0;
        } else {
            held = true;
        }
    } else {
        m_displayedSwr = rawSwr;
        m_hasReading = true;
        m_lowSwrCandidateSamples = 0;
        clearTimedConfirmations();
    }

    return {
        m_displayedSwr,
        m_forwardEnvelopeWatts,
        minimumForwardPower,
        held,
        m_hasReading,
    };
}

bool RadioSwrValidityFilter::confirmationElapsed(
    qint64& startedAtMs, qint64 timestampMs)
{
    if (startedAtMs < 0) {
        startedAtMs = timestampMs;
        return false;
    }
    return timestampMs - startedAtMs >= kTimedConfirmationMs;
}

void RadioSwrValidityFilter::clearTimedConfirmations()
{
    m_lowPowerRecoveryStartedAtMs = -1;
    m_belowUnityStartedAtMs = -1;
}

void RadioSwrValidityFilter::reset()
{
    m_displayedSwr = 1.0f;
    m_forwardEnvelopeWatts = 0.0f;
    m_lastTimestampMs = -1;
    clearTimedConfirmations();
    m_hasReading = false;
    m_lowSwrCandidateSamples = 0;
}

} // namespace AetherSDR
