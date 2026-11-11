#pragma once

#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QWidget>

#include <array>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;

namespace AetherSDR {
class ControlAvailabilityRegistry;
class IRadioBackend;
class RadioModel;

class NativePowerLimitsWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativePowerLimitsWidget(RadioModel& model, QWidget* parent = nullptr);

private:
    void bindBackend();
    void acceptStatus(const QVariantMap& state);
    void render();
    void querySelection();
    void submit(const QString& verb, QVariantMap fields);
    std::optional<int> confirmedWatts() const;

    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QVariantMap m_confirmed;
    QComboBox* m_type;
    QComboBox* m_band;
    QSpinBox* m_watts;
    QCheckBox* m_enabled;
    QLabel* m_readback;
    QLabel* m_status;
    bool m_seeded = false;
    quint64 m_bindingGeneration = 0;
    quint64 m_getRequest = 0;
    quint64 m_query = 0;
    quint64 m_edit = 0;
    std::array<QMetaObject::Connection, 3> m_connections;
};
} // namespace AetherSDR
