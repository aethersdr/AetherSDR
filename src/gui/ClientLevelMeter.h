#pragma once

#include <QWidget>

class QLabel;

namespace MasterSDR {

// Vertical level meter — gradient-filled bar with dB scale labels and a
// numeric readout below.  Same visual language as the meter half of
// ClientEqOutputFader, without the fader handle / drag interaction.
//
// Drive it from any UI-thread polling timer:
//   meter->setPeakDb(eq->outputPeakDb());
// or for a linear value:
//   meter->setPeakLinear(0.5f);
class ClientLevelMeter : public QWidget {
    Q_OBJECT

public:
    explicit ClientLevelMeter(QWidget* parent = nullptr);

    // Header label — defaults to "OUT".  Empty string hides the header.
    void setHeaderText(const QString& text);

    // Update the meter.  Both apply the same fast-attack / slow-release
    // smoothing as the EQ output fader so the visual feel matches.
    void setPeakDb(float peakDb);
    void setPeakLinear(float peakLinear);

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    QString m_headerText{"OUT"};
    float   m_smoothedPeak{-120.0f};

    static constexpr float kMeterMinDb = -60.0f;
    static constexpr float kMeterMaxDb =   0.0f;
    static constexpr int   kLabelColW  = 20;
    static constexpr int   kGap        = 2;
    static constexpr int   kBarW       = 16;
    static constexpr int   kStripTopPad    = 4;
    static constexpr int   kStripBottomPad = 4;
};

} // namespace MasterSDR
