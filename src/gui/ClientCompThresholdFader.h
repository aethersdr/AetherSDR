#pragma once

#include <QWidget>

class QLineEdit;

namespace MasterSDR {

// Combined input-level meter + threshold fader for the compressor
// editor.  Visual + interaction pattern mirrors ClientEqOutputFader —
// one custom-painted strip with a peak-level gradient fill plus a
// horizontal handle overhanging both sides of the bar.  Dragging the
// handle emits thresholdChanged(db); that same value is what the
// threshold chevron on the curve canvas represents, so the two
// controls stay in lockstep via the shared ClientComp state.
//
// Level scale is absolute dBFS [-60, 0], matching ClientCompMeter's
// Level mode.  Handle range is the same: you can pull the threshold
// anywhere the input meter can display.
class ClientCompThresholdFader : public QWidget {
    Q_OBJECT

public:
    explicit ClientCompThresholdFader(QWidget* parent = nullptr);

    void setThresholdDb(float db);
    float thresholdDb() const { return m_thresholdDb; }

    // Feed the latest input peak (dBFS).  Peak-follower smoothing is
    // applied internally so the bar doesn't flicker on silent frames.
    void setInputPeakDb(float db);

signals:
    void thresholdChanged(float db);

protected:
    void paintEvent(QPaintEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    void wheelEvent(QWheelEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void refreshValueLabel();
    void commitValueEdit();
    void setThresholdFromY(int y);

    QLineEdit* m_valueEdit{nullptr};
    float   m_thresholdDb{-18.0f};
    float   m_smoothedPeakDb{-120.0f};
    bool    m_dragging{false};

    static constexpr float kMeterMinDb = -60.0f;
    static constexpr float kMeterMaxDb =   0.0f;
    static constexpr float kThreshMinDb = -60.0f;
    static constexpr float kThreshMaxDb =   0.0f;
    static constexpr float kThreshDefaultDb = -18.0f;

    static constexpr int kLabelColW      = 22;
    static constexpr int kGap            = 2;
    static constexpr int kBarW           = 16;
    static constexpr int kHandleOverhang = 4;
    static constexpr int kHandleH        = 3;
    static constexpr int kStripTopPad    = 4;
    static constexpr int kStripBottomPad = 4;

    int m_stripTop{0};
    int m_stripH{0};
};

} // namespace MasterSDR
