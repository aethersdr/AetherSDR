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

// The dBm encoding a frame was decoded with, as the widget knew it then.
struct FrameEncoder {
    bool valid{false};
    float minDbm{0.0f};
    float maxDbm{0.0f};
    int yPixels{0};
};

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
    // A guard, not a measured round trip: how long a sent request whose
    // echo never matches it keeps the reply refused.
    static constexpr qint64 kRequestEchoGuardMs = 2000;

    using Encoder = FrameEncoder;

    // Every change of the displayed or requested centre or bandwidth.
    void noteGeometryChange() { m_framesAtGeometryChange = m_framesIngested; }

    // The display was cleared: no frame until the next one arrives.
    void clear() { *this = FrameCapture{}; }

    // When the radio must confirm geometry, a sent request changes nothing
    // until its echo. Until then (or kRequestEchoGuardMs) the reply refuses.
    void noteRequestSent(double centerMhz, double bandwidthMhz)
    {
        noteGeometryChange();
        m_requestCenterMhz = centerMhz;
        m_requestBandwidthMhz = bandwidthMhz;
        m_requestSent.start();
    }
    void noteRangeObserved(double centerMhz, double bandwidthMhz)
    {
        if (m_requestSent.isValid() && std::abs(centerMhz - m_requestCenterMhz) <= 1.0e-6
            && std::abs(bandwidthMhz - m_requestBandwidthMhz) <= 1.0e-6) {
            m_requestSent.invalidate();
        }
    }
    void cancelRequest() { m_requestSent.invalidate(); }
    bool requestUnconfirmed() const
    {
        return m_requestSent.isValid() && m_requestSent.elapsed() < kRequestEchoGuardMs;
    }

    // rebasedForDisplay: the widget judged this frame encoded with a dBm
    // range other than the one it was decoded with.
    void ingest(const QVector<float>& bins, quint64 fftFrameSequence,
                double centerMhz, double bandwidthMhz, bool rebasedForDisplay,
                const FrameEncoder& encoder = FrameEncoder())
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
        m_encoder = encoder;
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
        if (requestUnconfirmed()) {
            return QStringLiteral("a requested pan or zoom is not yet confirmed by the radio");
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

    // The frame with its ingest geometry and encoding. ageMs: monotonic time
    // since it arrived.
    QVariantMap snapshot() const
    {
        QVariantMap m = SpectrumTraceBins::snapshot(m_bins, m_centerMhz, m_bandwidthMhz,
                                                    m_fftFrameSequence);
        m[QStringLiteral("ageMs")] = m_received.isValid() ? m_received.elapsed() : qint64(-1);
        m[QStringLiteral("settledFrames")] = QVariant::fromValue(settledFrames());
        m[QStringLiteral("encoderRangeValid")] = m_encoder.valid;
        m[QStringLiteral("encoderMinDbm")] = m_encoder.minDbm;
        m[QStringLiteral("encoderMaxDbm")] = m_encoder.maxDbm;
        m[QStringLiteral("encoderYPixels")] = m_encoder.yPixels;
        return m;
    }

private:
    QVector<float> m_bins;
    quint64 m_fftFrameSequence{0};
    double m_centerMhz{0.0};
    double m_bandwidthMhz{0.0};
    bool m_rebasedForDisplay{false};
    FrameEncoder m_encoder;
    quint64 m_framesIngested{0};
    quint64 m_framesAtGeometryChange{0};
    QElapsedTimer m_received;
    double m_requestCenterMhz{0.0};
    double m_requestBandwidthMhz{0.0};
    QElapsedTimer m_requestSent;
};

}  // namespace AetherSDR::SpectrumTraceBins
