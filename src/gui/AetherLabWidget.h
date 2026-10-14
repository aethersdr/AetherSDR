#pragma once

#include "CanonSurface.h"
#include <QHash>

class QBoxLayout;
class QResizeEvent;
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

class AetherLabWidget final : public CanonCanvas {
    Q_OBJECT
public:
    explicit AetherLabWidget(AntennaAnalyzerModel* model, QWidget* parent = nullptr);
signals:
    void returnRequested();
protected:
    void resizeEvent(QResizeEvent* event) override;
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
    QLabel* m_frequency{nullptr};
    QLabel* m_quality{nullptr};
    QHash<QString, QLabel*> m_values;
    QBoxLayout* m_controls{nullptr};
    QBoxLayout* m_analysis{nullptr};
    QBoxLayout* m_models{nullptr};
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
