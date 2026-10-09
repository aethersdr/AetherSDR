#pragma once

#include <QString>
#include <QtGlobal>

namespace AetherSDR {

// Rate limit for PanadapterStream's VITA-49 sequence-error log line: at most one
// line per stream per kIntervalMs. The first error reports at once; later ones
// are held and reported with the first error after the interval, so only the
// error path touches this. Errors held when a stream goes clean are not logged.
class VitaSequenceLossLimiter {
public:
    static constexpr qint64 kIntervalMs = 10000;

    // Call on the first packet of each instance of the stream: the next
    // report's window starts here, and errors held from a previous instance
    // are dropped. The interval since the last report still applies, so a
    // stream torn down and re-created over and over logs no more often.
    void start(qint64 nowMs) { m_windowStartMs = nowMs; m_pending = 0; }

    // Count one sequence error. Returns true when a line is due; the line's
    // values are then reportErrors() and reportWindowMs().
    bool recordError(qint64 nowMs)
    {
        if (m_windowStartMs < 0) {
            m_windowStartMs = nowMs;
        }
        ++m_pending;
        if (m_lastReportMs >= 0 && nowMs - m_lastReportMs < kIntervalMs) {
            return false;
        }
        m_reportErrors = m_pending;
        m_reportWindowMs = nowMs - m_windowStartMs;
        m_pending = 0;
        m_lastReportMs = nowMs;
        m_windowStartMs = nowMs;
        return true;
    }

    int reportErrors() const { return m_reportErrors; }
    qint64 reportWindowMs() const { return m_reportWindowMs; }

private:
    qint64 m_windowStartMs{-1};
    qint64 m_lastReportMs{-1};
    int m_pending{0};
    int m_reportErrors{0};
    qint64 m_reportWindowMs{0};
};

// The log line the docs Log Analyzer matches on its fixed prefix. A sequence
// error is a gap in the 4-bit VITA-49 packet count, so it counts packets that
// were lost and packets that arrived out of order alike, and one gap is one
// error however many packets it skipped: the counts are errors, and the packet
// count is the packets received.
inline QString formatVitaSequenceLoss(const QString& category, quint32 streamId,
                                      int errors, qint64 windowMs,
                                      int totalErrors, int totalPackets)
{
    const qint64 seconds = (windowMs + 500) / 1000;
    return QStringLiteral("PanadapterStream: VITA-49 sequence errors on %1 stream 0x%2: "
                          "%3 in the last %4 s, %5 since the stream started "
                          "(%6 packets received)")
        .arg(category, QString::number(streamId, 16))
        .arg(errors)
        .arg(seconds)
        .arg(totalErrors)
        .arg(totalPackets);
}

} // namespace AetherSDR
