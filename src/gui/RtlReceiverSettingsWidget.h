#pragma once

#include <QPointer>
#include <QVariantMap>
#include <QWidget>

class QCheckBox;
class QGroupBox;
class QLabel;
class QSpinBox;

namespace AetherSDR {
class RadioModel;
class IRadioBackend;
class ControlAvailabilityRegistry;

// A vendor-extension client. Hardware/DSP adoption and settings ownership stay
// behind RadioModel; the widget never writes calibration or predicts readback.
class RtlReceiverSettingsWidget final : public QWidget {
    Q_OBJECT
public:
    explicit RtlReceiverSettingsWidget(RadioModel& model, QWidget* parent = nullptr);

private:
    void bindBackend();
    void queryStatus();
    void acceptStatus(const QVariantMap& status);
    void submit(const QString& verb, const QVariant& value, quint64& pending);
    void renderStatus();

    RadioModel& m_model;
    QPointer<IRadioBackend> m_backend;
    QMetaObject::Connection m_statusConnection;
    QMetaObject::Connection m_resultConnection;
    QMetaObject::Connection m_errorConnection;
    ControlAvailabilityRegistry* m_availability;
    QGroupBox* m_controls;
    QSpinBox* m_ppm;
    QCheckBox* m_dc;
    QLabel* m_applied;
    QLabel* m_status;
    QLabel* m_identity;
    QVariantMap m_confirmed;
    QString m_error;
    bool m_haveState = false;
    bool m_ppmEdited = false;
    quint64 m_bindingGeneration = 0;
    quint64 m_queryRequest = 0;
    quint64 m_ppmRequest = 0;
    quint64 m_dcRequest = 0;
};
} // namespace AetherSDR
