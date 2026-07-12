#include "DigitalVoiceWaveformTelemetry.h"

#include <QHash>

#include <cmath>
#include <limits>

namespace AetherSDR {

namespace {

constexpr char kMetricPrefix[] = "AETHER_DV_METRIC ";

bool parseBoundedDouble(const QHash<QByteArray, QByteArray>& fields,
                        const QByteArray& name,
                        double minimum,
                        double maximum,
                        double* value)
{
    if (!value || !fields.contains(name)) {
        return false;
    }
    bool ok = false;
    const double parsed = fields.value(name).toDouble(&ok);
    if (!ok || !std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = parsed;
    return true;
}

template<typename T>
bool parseBoundedUnsigned(const QHash<QByteArray, QByteArray>& fields,
                          const QByteArray& name,
                          quint64 maximum,
                          T* value)
{
    if (!value || !fields.contains(name)) {
        return false;
    }
    bool ok = false;
    const quint64 parsed = fields.value(name).toULongLong(&ok);
    if (!ok || parsed > maximum
        || parsed > static_cast<quint64>(std::numeric_limits<T>::max())) {
        return false;
    }
    *value = static_cast<T>(parsed);
    return true;
}

} // namespace

bool isDegradedDigitalVoiceWaveformHealth(DigitalVoiceWaveformHealth health)
{
    return health == DigitalVoiceWaveformHealth::CadenceDegraded
        || health == DigitalVoiceWaveformHealth::TransportLoss
        || health == DigitalVoiceWaveformHealth::SourceDeficits;
}

void DigitalVoiceWaveformHealthTracker::reset()
{
    m_health = DigitalVoiceWaveformHealth::Inactive;
    m_observationCount = 0;
    m_lowCadenceStreak = 0;
    m_sourceDeficitStreak = 0;
    m_healthyStreak = 0;
}

DigitalVoiceWaveformHealth DigitalVoiceWaveformHealthTracker::observe(
    const DigitalVoiceWaveformMetrics& metrics)
{
    if (!metrics.valid) {
        reset();
        return m_health;
    }

    ++m_observationCount;
    const bool wasDegraded = isDegradedDigitalVoiceWaveformHealth(m_health);

    if (metrics.vitaSequenceGaps > 0U) {
        m_lowCadenceStreak = 0;
        m_sourceDeficitStreak = 0;
        m_healthyStreak = 0;
        m_health = DigitalVoiceWaveformHealth::TransportLoss;
        return m_health;
    }

    if (metrics.sourceBlockDeficits > 0U) {
        m_lowCadenceStreak = 0;
        ++m_sourceDeficitStreak;
        m_healthyStreak = 0;
        if (m_sourceDeficitStreak >= kSourceDeficitWindows) {
            m_health = DigitalVoiceWaveformHealth::SourceDeficits;
        } else if (!wasDegraded && m_health == DigitalVoiceWaveformHealth::Inactive) {
            m_health = DigitalVoiceWaveformHealth::Measuring;
        }
        return m_health;
    }

    m_sourceDeficitStreak = 0;
    if (metrics.rxSampleRateHz < kCadenceWarningRateHz) {
        ++m_lowCadenceStreak;
        m_healthyStreak = 0;
        if (m_lowCadenceStreak >= kLowCadenceWindows) {
            m_health = DigitalVoiceWaveformHealth::CadenceDegraded;
        } else if (!wasDegraded && m_health == DigitalVoiceWaveformHealth::Inactive) {
            m_health = DigitalVoiceWaveformHealth::Measuring;
        }
        return m_health;
    }

    m_lowCadenceStreak = 0;
    ++m_healthyStreak;
    if (wasDegraded && m_healthyStreak < kRecoveryWindows) {
        return m_health;
    }

    m_health = m_observationCount >= kLowCadenceWindows
        ? DigitalVoiceWaveformHealth::Healthy
        : DigitalVoiceWaveformHealth::Measuring;
    return m_health;
}

QList<QByteArray> DigitalVoiceWaveformTelemetryParser::append(const QByteArray& chunk)
{
    QByteArray remaining = chunk;
    if (m_discardUntilNewline) {
        const qsizetype newline = remaining.indexOf('\n');
        if (newline < 0) {
            return {};
        }
        remaining.remove(0, newline + 1);
        m_discardUntilNewline = false;
    }

    m_buffer.append(remaining);
    QList<QByteArray> lines = takeCompleteLines();
    if (m_buffer.size() > kMaximumBufferedLineBytes) {
        m_buffer.clear();
        m_discardUntilNewline = true;
    }
    return lines;
}

QList<QByteArray> DigitalVoiceWaveformTelemetryParser::finish()
{
    QList<QByteArray> lines = takeCompleteLines();
    if (!m_discardUntilNewline && !m_buffer.isEmpty()) {
        if (m_buffer.endsWith('\r')) {
            m_buffer.chop(1);
        }
        lines.append(m_buffer);
    }
    reset();
    return lines;
}

void DigitalVoiceWaveformTelemetryParser::reset()
{
    m_buffer.clear();
    m_discardUntilNewline = false;
}

bool DigitalVoiceWaveformTelemetryParser::isMetricLine(const QByteArray& line)
{
    return line.startsWith(kMetricPrefix);
}

bool DigitalVoiceWaveformTelemetryParser::parseMetricLine(
    const QByteArray& line,
    DigitalVoiceWaveformMetrics* metrics)
{
    if (!metrics || line.size() > kMaximumMetricLineBytes
        || !isMetricLine(line)) {
        return false;
    }

    const QList<QByteArray> tokens =
        line.mid(static_cast<qsizetype>(sizeof(kMetricPrefix) - 1U))
            .split(' ');
    QHash<QByteArray, QByteArray> fields;
    for (const QByteArray& token : tokens) {
        if (token.isEmpty()) {
            continue;
        }
        const qsizetype equals = token.indexOf('=');
        if (equals <= 0 || equals == token.size() - 1) {
            return false;
        }
        const QByteArray key = token.left(equals);
        if (fields.contains(key)) {
            return false;
        }
        fields.insert(key, token.mid(equals + 1));
    }

    const QByteArray version = fields.value("v");
    if (version != "1" && version != "2" && version != "3") {
        return false;
    }

    DigitalVoiceWaveformMetrics parsed;
    const QByteArray mode = fields.value("mode");
    if (mode.isEmpty() || mode.size() > 16) {
        return false;
    }
    for (const char ch : mode) {
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) {
            return false;
        }
    }
    parsed.mode = QString::fromLatin1(mode);

    const QByteArray direction = version == "1"
        ? QByteArrayLiteral("RX")
        : fields.value("dir");
    if (direction == "RX") {
        parsed.direction = DigitalVoiceWaveformMetricDirection::Rx;
        const QByteArray rateField = version == "1"
            ? QByteArrayLiteral("rx_hz")
            : QByteArrayLiteral("rate_hz");
        if (!parseBoundedDouble(fields, rateField, 1.0, 96000.0,
                                &parsed.rxSampleRateHz)
            || !parseBoundedUnsigned(fields, "vita_gaps", 1000000U,
                                     &parsed.vitaSequenceGaps)
            || !parseBoundedUnsigned(fields, "source_blocks", 1000000U,
                                     &parsed.sourceBlockDeficits)
            || !parseBoundedDouble(fields, "turn_mean_us", 0.0, 60000000.0,
                                   &parsed.turnaroundMeanUs)
            || !parseBoundedUnsigned(fields, "turn_max_us", 60000000U,
                                     &parsed.turnaroundMaxUs)
            || !parseBoundedUnsigned(fields, "queue_max", 1000000U,
                                     &parsed.queueMax)) {
            return false;
        }
    } else if (direction == "TX" && (version == "2" || version == "3")) {
        parsed.direction = DigitalVoiceWaveformMetricDirection::Tx;
        parsed.txValid = true;
        if (!parseBoundedDouble(fields, "rate_hz", 0.0, 96000.0,
                                &parsed.txSampleRateHz)
            || !parseBoundedUnsigned(fields, "vita_gaps", 1000000U,
                                     &parsed.txVitaSequenceGaps)
            || !parseBoundedUnsigned(fields, "null_frames", 1000000U,
                                     &parsed.txNullFrames)
            || !parseBoundedUnsigned(fields, "pcm_clips", 1000000U,
                                     &parsed.txPcmClips)
            || !parseBoundedUnsigned(fields, "pcm_invalid", 1000000U,
                                     &parsed.txPcmInvalid)
            || !parseBoundedUnsigned(fields, "send_failures", 1000000U,
                                     &parsed.txSendFailures)
            || !parseBoundedUnsigned(fields, "queue_max", 1000000U,
                                     &parsed.txQueueMax)
            || !parseBoundedUnsigned(fields, "tail_samples", 1000000U,
                                     &parsed.txTailSamples)
            || !parseBoundedUnsigned(fields, "tail_us", 60000000U,
                                     &parsed.txTailUs)) {
            return false;
        }
        if (version == "3"
            && (!parseBoundedUnsigned(fields, "preroll_frames", 1000000U,
                                      &parsed.txPreRollFrames)
                || !parseBoundedUnsigned(fields, "preroll_delay_ms", 60000U,
                                         &parsed.txPreRollDelayMs)
                || !parseBoundedUnsigned(fields, "ambe_queue_max", 1000000U,
                                         &parsed.txAmbeQueueMax)
                || !parseBoundedUnsigned(fields, "ambe_underflows", 1000000U,
                                         &parsed.txAmbeUnderflows)
                || !parseBoundedUnsigned(fields, "ambe_overflows", 1000000U,
                                         &parsed.txAmbeOverflows)
                || !parseBoundedUnsigned(fields, "ambe_sequence_errors", 1000000U,
                                         &parsed.txAmbeSequenceErrors)
                || !parseBoundedUnsigned(fields, "vocoder_submit_failures", 1000000U,
                                         &parsed.txVocoderSubmitFailures)
                || !parseBoundedUnsigned(fields, "vocoder_pending_max", 1000000U,
                                         &parsed.txVocoderPendingMax)
                || !parseBoundedUnsigned(fields, "drain_frames", 1000000U,
                                         &parsed.txDrainFrames)
                || !parseBoundedUnsigned(fields, "drain_timeouts", 1000000U,
                                         &parsed.txDrainTimeouts)
                || !parseBoundedUnsigned(fields, "drain_discarded_frames", 1000000U,
                                         &parsed.txDrainDiscardedFrames))) {
            return false;
        }
    } else {
        return false;
    }

    parsed.valid = true;
    *metrics = parsed;
    return true;
}

QList<QByteArray> DigitalVoiceWaveformTelemetryParser::takeCompleteLines()
{
    QList<QByteArray> lines;
    qsizetype newline = m_buffer.indexOf('\n');
    while (newline >= 0) {
        QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        lines.append(line);
        newline = m_buffer.indexOf('\n');
    }
    return lines;
}

} // namespace AetherSDR
