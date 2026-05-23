#pragma once

#include <QWidget>

class QPushButton;
class QLabel;
class QSlider;
class ScrollableLabel;

namespace MasterSDR {

class TransmitModel;

// PHONE applet — phone voice TX controls matching the SmartSDR Phone panel.
//
// Layout (top to bottom):
//  - Title bar: "PHONE"
//  - AM Carrier level slider
//  - VOX toggle + level slider
//  - VOX delay slider
//  - DEXP toggle + level slider (non-functional on fw v1.4.0.0)
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
    QSlider* m_amCarrierSlider{nullptr};
    QLabel*  m_amCarrierLabel{nullptr};

    // VOX
    QPushButton* m_voxBtn{nullptr};
    QSlider*     m_voxLevelSlider{nullptr};
    QLabel*      m_voxLevelLabel{nullptr};

    // VOX delay
    QSlider* m_voxDelaySlider{nullptr};
    QLabel*  m_voxDelayLabel{nullptr};

    // DEXP (non-functional on fw v1.4.0.0 — see GitHub issue)
    QPushButton* m_dexpBtn{nullptr};
    QSlider*     m_dexpSlider{nullptr};
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

} // namespace MasterSDR
