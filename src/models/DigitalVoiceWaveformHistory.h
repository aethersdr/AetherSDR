#pragma once

#include "core/DigitalVoiceWaveformTelemetry.h"

namespace AetherSDR {

struct DigitalVoiceWaveformHistoryReading {
    bool valid{false};
    bool rxValid{false};
    bool txValid{false};
    double rxSampleRateHz{0.0};
    double vitaSequenceGapsPerSecond{0.0};
    double sourceBlockDeficitsPerSecond{0.0};
    double turnaroundMeanUs{0.0};
    quint64 turnaroundMaxUs{0};
    quint32 queueMax{0};
    double txSampleRateHz{0.0};
    double txVitaSequenceGapsPerSecond{0.0};
    quint32 txNullFrames{0};
    quint32 txPcmClips{0};
    quint32 txPcmInvalid{0};
    quint32 txSendFailures{0};
    quint32 txQueueMax{0};
    quint32 txTailSamples{0};
    quint64 txTailUs{0};
};

class DigitalVoiceWaveformHistoryTracker
{
public:
    static constexpr qint64 kTelemetryStaleMs = 3500;

    DigitalVoiceWaveformHistoryReading sample(const DigitalVoiceWaveformMetrics& metrics,
                                       qint64 nowMs,
                                       double elapsedSeconds);
    void reset();

private:
    bool m_haveBaseline{false};
    quint64 m_generation{0};
    quint64 m_lastVitaSequenceGapsTotal{0};
    quint64 m_lastSourceBlockDeficitsTotal{0};
    qint64 m_lastMetricsTimestampMs{0};
    bool m_haveTxBaseline{false};
    quint64 m_lastTxVitaSequenceGapsTotal{0};
    qint64 m_lastTxMetricsTimestampMs{0};
};

} // namespace AetherSDR
