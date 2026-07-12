#include "DigitalVoiceWaveformHistory.h"

#include <algorithm>

namespace AetherSDR {

DigitalVoiceWaveformHistoryReading DigitalVoiceWaveformHistoryTracker::sample(
    const DigitalVoiceWaveformMetrics& metrics,
    qint64 nowMs,
    double elapsedSeconds)
{
    DigitalVoiceWaveformHistoryReading reading;
    if (!metrics.valid && !metrics.txValid) {
        reset();
        return reading;
    }

    if (m_generation != metrics.generation) {
        reset();
        m_generation = metrics.generation;
    }

    if (metrics.valid) {
        const qint64 ageMs = nowMs - metrics.timestampMs;
        if (ageMs >= 0 && ageMs <= kTelemetryStaleMs) {
            reading.valid = true;
            reading.rxValid = true;
            reading.rxSampleRateHz = metrics.rxSampleRateHz;
            reading.turnaroundMeanUs = metrics.turnaroundMeanUs;
            reading.turnaroundMaxUs = metrics.turnaroundMaxUs;
            reading.queueMax = metrics.queueMax;

            double metricElapsedSeconds = std::max(0.001, elapsedSeconds);
            quint64 gapDelta = 0;
            quint64 sourceDelta = 0;
            if (!m_haveBaseline) {
                m_haveBaseline = true;
                gapDelta = metrics.vitaSequenceGaps;
                sourceDelta = metrics.sourceBlockDeficits;
            } else {
                gapDelta = metrics.vitaSequenceGapsTotal
                        >= m_lastVitaSequenceGapsTotal
                    ? metrics.vitaSequenceGapsTotal - m_lastVitaSequenceGapsTotal
                    : metrics.vitaSequenceGapsTotal;
                sourceDelta = metrics.sourceBlockDeficitsTotal
                        >= m_lastSourceBlockDeficitsTotal
                    ? metrics.sourceBlockDeficitsTotal
                        - m_lastSourceBlockDeficitsTotal
                    : metrics.sourceBlockDeficitsTotal;
                if (metrics.timestampMs > m_lastMetricsTimestampMs) {
                    metricElapsedSeconds =
                        (metrics.timestampMs - m_lastMetricsTimestampMs) / 1000.0;
                }
            }
            reading.vitaSequenceGapsPerSecond =
                static_cast<double>(gapDelta) / metricElapsedSeconds;
            reading.sourceBlockDeficitsPerSecond =
                static_cast<double>(sourceDelta) / metricElapsedSeconds;

            m_lastVitaSequenceGapsTotal = metrics.vitaSequenceGapsTotal;
            m_lastSourceBlockDeficitsTotal = metrics.sourceBlockDeficitsTotal;
            m_lastMetricsTimestampMs = metrics.timestampMs;
        }
    }

    if (metrics.txValid) {
        const qint64 ageMs = nowMs - metrics.txTimestampMs;
        if (ageMs >= 0 && ageMs <= kTelemetryStaleMs) {
            reading.valid = true;
            reading.txValid = true;
            reading.txSampleRateHz = metrics.txSampleRateHz;
            reading.txNullFrames = metrics.txNullFrames;
            reading.txPcmClips = metrics.txPcmClips;
            reading.txPcmInvalid = metrics.txPcmInvalid;
            reading.txSendFailures = metrics.txSendFailures;
            reading.txQueueMax = metrics.txQueueMax;
            reading.txTailSamples = metrics.txTailSamples;
            reading.txTailUs = metrics.txTailUs;

            double metricElapsedSeconds = std::max(0.001, elapsedSeconds);
            quint64 gapDelta = metrics.txVitaSequenceGaps;
            if (m_haveTxBaseline) {
                gapDelta = metrics.txVitaSequenceGapsTotal
                        >= m_lastTxVitaSequenceGapsTotal
                    ? metrics.txVitaSequenceGapsTotal
                        - m_lastTxVitaSequenceGapsTotal
                    : metrics.txVitaSequenceGapsTotal;
                if (metrics.txTimestampMs > m_lastTxMetricsTimestampMs) {
                    metricElapsedSeconds =
                        (metrics.txTimestampMs - m_lastTxMetricsTimestampMs)
                        / 1000.0;
                }
            }
            reading.txVitaSequenceGapsPerSecond =
                static_cast<double>(gapDelta) / metricElapsedSeconds;
            m_haveTxBaseline = true;
            m_lastTxVitaSequenceGapsTotal = metrics.txVitaSequenceGapsTotal;
            m_lastTxMetricsTimestampMs = metrics.txTimestampMs;
        }
    }
    return reading;
}

void DigitalVoiceWaveformHistoryTracker::reset()
{
    m_haveBaseline = false;
    m_generation = 0;
    m_lastVitaSequenceGapsTotal = 0;
    m_lastSourceBlockDeficitsTotal = 0;
    m_lastMetricsTimestampMs = 0;
    m_haveTxBaseline = false;
    m_lastTxVitaSequenceGapsTotal = 0;
    m_lastTxMetricsTimestampMs = 0;
}

} // namespace AetherSDR
