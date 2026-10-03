#pragma once

#include <QtGlobal>

namespace AetherSDR {

// Validates the radio's SWR stream against instantaneous forward power: the
// power envelope attacks immediately and releases with time, so modulation
// gaps cannot pull SWR to 1 while a sustained lower power eventually counts.
// This is a trust gate, not display smoothing (displayed SWR passes through
// unchanged), so AGENTS.md's MeterSmoother rule does not apply.
class RadioSwrValidityFilter {
public:
    struct Result {
        float displayedSwr{1.0f};
        float forwardEnvelopeWatts{0.0f};
        float minimumForwardWatts{0.05f};
        bool held{false};
        bool hasReading{false};
    };

    Result update(float forwardPowerInstant, float rawSwr,
                  qint64 timestampMs, float maximumDisplayedSwr);
    void reset();

private:
    static constexpr float kMinimumForwardPowerWatts = 0.05f;
    static constexpr float kEnvelopeThresholdFraction = 0.20f;
    static constexpr float kLowSwrConfirmationMaximum = 1.2f;
    static constexpr int kLowSwrConfirmationSamples = 3;
    static constexpr qint64 kTimedConfirmationMs = 250;
    static constexpr double kEnvelopeHalfLifeMs = 750.0;

    static bool confirmationElapsed(qint64& startedAtMs, qint64 timestampMs);
    void clearTimedConfirmations();

    float m_displayedSwr{1.0f};
    float m_forwardEnvelopeWatts{0.0f};
    qint64 m_lastTimestampMs{-1};
    qint64 m_lowPowerRecoveryStartedAtMs{-1};
    qint64 m_belowUnityStartedAtMs{-1};
    bool m_hasReading{false};
    int m_lowSwrCandidateSamples{0};
};

} // namespace AetherSDR
