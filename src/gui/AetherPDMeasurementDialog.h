#pragma once

#include "CanonWindow.h"
#include "models/RadioModel.h"
#include "models/TxController.h"

#include <QPointer>
#include <QDateTime>
#include <memory>

class QCheckBox;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace AetherSDR {
class AetherPDMeasurementSpectrum;
class AetherPDMeasurementDialog final : public CanonWindow {
    Q_OBJECT
public:
    explicit AetherPDMeasurementDialog(RadioModel* radio, QWidget* parent = nullptr);
    ~AetherPDMeasurementDialog() override;
    void done(int result) override;

private:
    bool current() const;
    void render();
    void prepare();
    void transmit(const std::shared_ptr<TxController>& controller, const TxController::Input& input, quint64 preparationId);
    void stop();
    void exportResult();
    QPointer<RadioModel> m_radio;
    RadioControlSession m_session;
    std::shared_ptr<TxController> m_controller;
    bool m_closed{false};
    QString m_notice;
    QCheckBox* m_dummyLoad{nullptr};
    QDoubleSpinBox* m_limit{nullptr};
    QComboBox* m_window{nullptr};
    QSpinBox* m_averages{nullptr};
    QComboBox* m_order{nullptr};
    QLabel* m_state{nullptr};
    QPlainTextEdit* m_result{nullptr};
    AetherPDMeasurementSpectrum* m_spectrum{nullptr};
    QDateTime m_spectrumCompletedAt;
    quint64 m_spectrumSourceId{0};
    QPushButton* m_arm{nullptr};
    QPushButton* m_cancelSource{nullptr};
    QPushButton* m_prepare{nullptr};
    QPushButton* m_transmit{nullptr};
    QPushButton* m_stop{nullptr};
    QPushButton* m_export{nullptr};
};
} // namespace AetherSDR
