#pragma once

#include <QWidget>
#include <vector>

class QTimer;

namespace MasterSDR {

class ClientGate;

// Scrolling level-history view for the Gate editor — mirrors Ableton's
// Gate device layout: dB scale on the left, time running right-to-left
// with the newest sample at the right edge, input peak drawn as a
// white outline, gain-reduction overlaid as a dark-gray fill under
// the input, and a pair of cyan horizontal lines at threshold (upper)
// and threshold - return (lower) showing the hysteresis band.
//
// Sampling: the bound ClientGate's inputPeakDb() and
// gainReductionDb() are polled at ~30 Hz and pushed into a ring
// buffer (~2 s of history at 30 Hz ≈ 60 samples).  Low-freq signal
// so cheap to paint; the widget is the focal point of the editor.
class ClientGateLevelView : public QWidget {
    Q_OBJECT

public:
    explicit ClientGateLevelView(QWidget* parent = nullptr);

    void setGate(ClientGate* gate);

    // dB extents of the vertical axis.  Matches the editor label row.
    static constexpr float kTopDb    =   6.0f;
    static constexpr float kBottomDb = -70.0f;

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    void tick();

    float dbToY(float db) const;

    ClientGate* m_gate{nullptr};
    QTimer*     m_timer{nullptr};

    struct Sample {
        float inputDb;
        float grDb;
    };
    std::vector<Sample> m_history;
    int                 m_writeIdx{0};
};

} // namespace MasterSDR
