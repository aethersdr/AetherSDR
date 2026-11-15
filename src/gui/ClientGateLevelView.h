#pragma once

#include <QWidget>

class QHideEvent;
class QShowEvent;
#include <vector>

class QTimer;

namespace AetherSDR {

class ClientGate;

// Scrolling level history for the Gate editor (Ableton Gate layout): dB scale
// left, newest sample at the right, input peak as a white outline, gain
// reduction as dark-gray fill, cyan lines at threshold and threshold - return
// (the hysteresis band). inputPeakDb()/gainReductionDb() are polled at ~30 Hz
// into a ~60-sample (~2 s) ring.
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
    // Polling stops while this widget is hidden — a stacked page behind
    // another tab still gets its timer events, but not its repaints.
    void showEvent(QShowEvent* ev) override;
    void hideEvent(QHideEvent* ev) override;

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

} // namespace AetherSDR
