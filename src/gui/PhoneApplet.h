#pragma once

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

    bool m_updatingFromModel{false};
};

} // namespace AetherSDR
