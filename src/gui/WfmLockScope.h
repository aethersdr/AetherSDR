#pragma once

#include "models/SliceModel.h"

#include <QElapsedTimer>
#include <QList>
#include <QWidget>
#include <optional>

class QPainter;

namespace AetherSDR {

// Low-rate, measured pilot history. No decoder access, sampling timer or FFT.
// The applet clears it whenever its selected receiver observation is retired.
class WfmLockScope : public QWidget {
    Q_OBJECT
public:
    explicit WfmLockScope(QWidget* parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // Magnitude and both thresholds share the decoder's relative scale. This
    // is not calibrated RF power, SNR, PLL phase error or an audio level.
    void appendSample(double pilotMagnitude, double acquireThreshold,
                      double releaseThreshold, WfmStereoStatus status,
                      bool forceMono);
    // HD uses actual decoder MER values, with no invented lock threshold.
    void setHdMode(bool enabled);
    void appendHdSample(std::optional<double> lowerMerDb, std::optional<double> upperMerDb,
                        bool synced, bool audioValid, bool recentlyRecovered = false);
    void clear();
    int sampleCount() const { return int(m_hdMode ? m_hdSamples.size() : m_samples.size()); }

protected:
    void paintEvent(QPaintEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct Sample {
        qint64 milliseconds;
        double magnitude;
        double acquire;
        double release;
        WfmStereoStatus status;
        bool forceMono;
    };
    struct HdSample {
        qint64 milliseconds;
        std::optional<double> lower;
        std::optional<double> upper;
        bool synced;
        bool audioValid;
        bool recentlyRecovered;
    };
    void paintHd(QPainter& painter);
    bool m_hdMode{false};
    QList<HdSample> m_hdSamples;
    static constexpr int kCapacity = 160;
    static constexpr qint64 kWindowMs = 40000;
    QList<Sample> m_samples;
    QElapsedTimer m_clock;
};

} // namespace AetherSDR
