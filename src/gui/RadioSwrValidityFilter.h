#pragma once

#include <QtGlobal>

namespace AetherSDR {

// Validates the radio-native SWR stream against its instantaneous forward
// power. The forward-power envelope attacks immediately and releases with
// elapsed time, so brief modulation gaps cannot force SWR toward 1 while a
// sustained lower-power operating point eventually becomes authoritative.
//
// Note on AGENTS.md "use MeterSmoother": the forward-power envelope here is a
// trust GATE (is there enough measurable carrier to believe this SWR sample?),
// not display smoothing — it never smooths the number shown on the meter. The
// displayed SWR is passed through verbatim once the gate accepts it, so this is
// deliberately outside the MeterSmoother rule, which governs the value drawn.
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
