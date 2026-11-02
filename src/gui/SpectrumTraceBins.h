#pragma once

// The per-frame trace a SpectrumWidget draws from, for the bridge's
// `get tracedebug <pan> bins` (#5806). Kept free of QtWidgets so it can be
// tested without a widget.

#include <QElapsedTimer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QtGlobal>

#include <cmath>

namespace AetherSDR::SpectrumTraceBins {

// bins: the decoded frame before display smoothing and before any pan-edge
// crop, spread evenly across centerMhz ± bandwidthMhz/2. A non-finite bin is
// reported as null.
inline QVariantMap snapshot(const QVector<float>& bins, double centerMhz,
                            double bandwidthMhz, quint64 fftFrameSequence)
{
    const double spanHz = bandwidthMhz * 1e6;
    QVariantList values;
    values.reserve(bins.size());
    int finiteCount = 0;
    for (float v : bins) {
        if (std::isfinite(v)) {
            values.append(static_cast<double>(v));
            ++finiteCount;
        } else {
            values.append(QVariant());
        }
    }
    QVariantMap m;
    m[QStringLiteral("count")] = bins.size();
    m[QStringLiteral("finiteCount")] = finiteCount;
    m[QStringLiteral("centerHz")] = centerMhz * 1e6;
    m[QStringLiteral("spanHz")] = spanHz;
    m[QStringLiteral("lowHz")] = centerMhz * 1e6 - spanHz / 2.0;
    m[QStringLiteral("binHz")] = bins.isEmpty() ? 0.0 : spanHz / bins.size();
    m[QStringLiteral("fftFrameSequence")] = QVariant::fromValue(fftFrameSequence);
    m[QStringLiteral("bins")] = values;
    return m;
}

// The last decoded frame exactly as it arrived, with the pan geometry shown
// when it arrived. A frame carries no geometry of its own, so the pair is
// trusted only once kSettledFrames frames have arrived since the geometry
// last changed, and never while the pan is still moving.
class FrameCapture
{
public:
    // A settling guard, not a measured latency: how many frames the radio
    // still sends for the old geometry after a change is not known.
    static constexpr quint64 kSettledFrames = 3;

    // Every change of the displayed or requested centre or bandwidth.
    void noteGeometryChange() { m_framesAtGeometryChange = m_framesIngested; }

    // rebasedForDisplay: the widget judged this frame encoded with a dBm
    // range other than the one it was decoded with.
    void ingest(const QVector<float>& bins, quint64 fftFrameSequence,
                double centerMhz, double bandwidthMhz, bool rebasedForDisplay)
    {
        if (bins.isEmpty()) {
            return;
        }
        if (m_framesIngested > 0
            && (centerMhz != m_centerMhz || bandwidthMhz != m_bandwidthMhz)) {
            noteGeometryChange();  // a change no caller reported
        }
        ++m_framesIngested;
        m_bins = bins;
        m_fftFrameSequence = fftFrameSequence;
        m_centerMhz = centerMhz;
        m_bandwidthMhz = bandwidthMhz;
        m_rebasedForDisplay = rebasedForDisplay;
        m_received.start();
    }

    quint64 settledFrames() const { return m_framesIngested - m_framesAtGeometryChange; }

    // Empty when the frame and its geometry may be returned, else why not.
    QString refusal(double centerMhzNow, double bandwidthMhzNow,
                    bool panAnimationRunning, bool gestureInProgress) const
    {
        if (m_framesIngested == 0) {
            return QStringLiteral("no frame decoded yet");
        }
        if (panAnimationRunning) {
            return QStringLiteral("pan animation running");
        }
        if (gestureInProgress) {
            return QStringLiteral("a pan or zoom is in progress");
        }
        const QString wait = QStringLiteral("; wait for %1 settled frames").arg(kSettledFrames);
        if (centerMhzNow != m_centerMhz || bandwidthMhzNow != m_bandwidthMhz) {
            return QStringLiteral("geometry changed after the last frame") + wait;
        }
        if (settledFrames() < kSettledFrames) {
            return QStringLiteral("geometry changed %1 frames ago").arg(settledFrames()) + wait;
        }
        if (m_rebasedForDisplay) {
            return QStringLiteral("the last frame was decoded across a dBm range change; "
                                  "wait for a frame in the new encoding");
        }
        return {};
    }

    // The frame with its ingest geometry. ageMs: monotonic time since it arrived.
    QVariantMap snapshot() const
    {
        QVariantMap m = SpectrumTraceBins::snapshot(m_bins, m_centerMhz, m_bandwidthMhz,
                                                    m_fftFrameSequence);
        m[QStringLiteral("ageMs")] = m_received.isValid() ? m_received.elapsed() : qint64(-1);
        m[QStringLiteral("settledFrames")] = QVariant::fromValue(settledFrames());
        return m;
    }

private:
    QVector<float> m_bins;
    quint64 m_fftFrameSequence{0};
    double m_centerMhz{0.0};
    double m_bandwidthMhz{0.0};
    bool m_rebasedForDisplay{false};
    quint64 m_framesIngested{0};
    quint64 m_framesAtGeometryChange{0};
    QElapsedTimer m_received;
};

}  // namespace AetherSDR::SpectrumTraceBins
