#pragma once

#include <QWidget>
#include <array>
#include "models/EqualizerModel.h"

class QPushButton;
class QLabel;
class QSlider;

namespace AetherSDR {

// EQ applet — 8-band graphic equalizer for TX and RX.
//
// Layout:
//  - Title bar: "EQ"
//  - ON button + RX/TX selector
//  - Band labels: 63 | 125 | 250 | 500 | 1k | 2k | 4k | 8k
//  - 8 vertical sliders (-10 to +10 dB)
//  - dB scale labels on sides
class EqApplet : public QWidget {
    Q_OBJECT

public:
    explicit EqApplet(QWidget* parent = nullptr);

    void setEqualizerModel(EqualizerModel* model);

private:
    void buildUI();
    void syncFromModel();
    void updateActiveHighlight();
    void persistViewSelection();

    EqualizerModel* m_model{nullptr};
    bool m_updatingFromModel{false};
    bool m_showTx{true};  // true = TX view, false = RX view

    QPushButton* m_onBtn{nullptr};
    QPushButton* m_rxBtn{nullptr};
    QPushButton* m_txBtn{nullptr};

    QWidget* m_resetBtn{nullptr};

    std::array<QSlider*, EqualizerModel::BandCount> m_sliders{};
    std::array<QLabel*,  EqualizerModel::BandCount> m_valueLabels{};
};

} // namespace AetherSDR
