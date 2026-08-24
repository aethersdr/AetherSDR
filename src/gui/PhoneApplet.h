#pragma once

#include <QList>
#include <QWidget>

class QPushButton;
class QLabel;
class QSlider;
class GuardedSlider;
class ScrollableLabel;

namespace AetherSDR {

class TransmitModel;

// PHONE applet — phone voice TX controls matching the SmartSDR Phone panel.
//
// Layout (top to bottom):
//  - Title bar: "PHONE"
//  - AM Carrier level slider
//  - VOX toggle + level slider
//  - VOX delay slider
//  - DEXP toggle + level slider
//  - TX filter: Low Cut / High Cut step buttons
class PhoneApplet : public QWidget {
    Q_OBJECT

public:
    explicit PhoneApplet(QWidget* parent = nullptr);

    void setTransmitModel(TransmitModel* model);

    // Hide the complete DEXP row when the connected backend has no
    // authoritative downward-expander command path.
    void setDexpVisible(bool visible);

    // The TX passband edges the connected radio can actually reach, ascending
    // (RadioCapabilities::txFilterLowEdgesHz / txFilterHighEdgesHz). Empty
    // restores the continuous 50 Hz behaviour.
    //
    // WHY THE STEPPERS NEED THIS. An Icom stores four to six low edges and four
    // high ones and has nothing in between. Stepping 50 Hz at a time through
    // that meant the label moved on every click and the transmitter moved on
    // roughly one in ten — a control that looked fine and was mostly inert.
    // With the list, one click is one reachable edge.
    void setTxFilterEdges(const QList<int>& lowEdgesHz, const QList<int>& highEdgesHz);

private:
    void buildUI();
    void syncFromModel();

    TransmitModel* m_model{nullptr};

    // AM Carrier
    GuardedSlider* m_amCarrierSlider{nullptr};
    QLabel*  m_amCarrierLabel{nullptr};

    // VOX
    QPushButton* m_voxBtn{nullptr};
    GuardedSlider* m_voxLevelSlider{nullptr};
    QLabel*      m_voxLevelLabel{nullptr};

    // VOX delay
    QSlider* m_voxDelaySlider{nullptr};
    QLabel*  m_voxDelayLabel{nullptr};

    // DEXP (radio compander control)
    QWidget*     m_dexpRow{nullptr};
    QPushButton* m_dexpBtn{nullptr};
    GuardedSlider* m_dexpSlider{nullptr};
    QLabel*      m_dexpLabel{nullptr};

    // TX filter
    QSlider* m_lowCutSlider{nullptr};
    ScrollableLabel* m_lowCutLabel{nullptr};
    QPushButton* m_lowCutDown{nullptr};
    QPushButton* m_lowCutUp{nullptr};

    QSlider* m_highCutSlider{nullptr};
    ScrollableLabel* m_highCutLabel{nullptr};
    QPushButton* m_highCutDown{nullptr};
    QPushButton* m_highCutUp{nullptr};

    // Empty = the radio takes a continuous passband, or nothing is connected.
    QList<int> m_txLowEdgesHz;
    QList<int> m_txHighEdgesHz;

    // One click of a stepper. Walks `edges` when it has any, and falls back to
    // the 50 Hz snap otherwise. `dir` is -1 down, +1 up.
    [[nodiscard]] static int steppedEdgeHz(const QList<int>& edges, int currentHz, int dir);

    bool m_updatingFromModel{false};
};

} // namespace AetherSDR
