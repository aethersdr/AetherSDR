#pragma once

#include "CanonWindow.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;

namespace AetherSDR {
class AntennaAnalyzerModel;
class AetherLabPlot;

class AetherLabWindow final : public CanonWindow {
    Q_OBJECT
public:
    explicit AetherLabWindow(AntennaAnalyzerModel* model, QWidget* parent = nullptr);
protected:
    void hideEvent(QHideEvent* event) override;
private:
    void refresh();
    void saveSession();
    AntennaAnalyzerModel* m_model;
    AetherLabPlot* m_plot{nullptr};
    QComboBox* m_devices{nullptr};
    QDoubleSpinBox* m_start{nullptr};
    QDoubleSpinBox* m_stop{nullptr};
    QSpinBox* m_intervals{nullptr};
    QCheckBox* m_isolated{nullptr};
    QSlider* m_marker{nullptr};
    QLabel* m_status{nullptr};
    QLabel* m_readout{nullptr};
    QPushButton* m_rescan{nullptr};
    QPushButton* m_disconnect{nullptr};
    QPushButton* m_minimum{nullptr};
    QPushButton* m_connect{nullptr};
    QPushButton* m_sweep{nullptr};
    QPushButton* m_cancel{nullptr};
    QPushButton* m_save{nullptr};
    QTimer* m_refreshTimer{nullptr};
    quint64 m_generation{0};
    QString m_deviceSignature;
};
} // namespace AetherSDR
