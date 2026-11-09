#pragma once

#include <QMetaObject>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>
#include <QWidget>

#include <array>

class QComboBox;
class QLabel;

namespace AetherSDR {
class ControlAvailabilityRegistry;
class IRadioBackend;
class RadioModel;

class NativeTransmitInputsWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativeTransmitInputsWidget(RadioModel& model, QWidget* parent = nullptr);
private:
    void bindBackend();
    void acceptStatus(const QVariantMap& status);
    void render();
    void submit(int path, int selection);

    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QStringList m_choices;
    QVariantMap m_confirmed;
    std::array<QComboBox*, 2> m_inputs;
    QLabel* m_readback;
    QLabel* m_status;
    quint64 m_query = 0;
    quint64 m_edit = 0;
    quint64 m_bindingGeneration = 0;
    std::array<QMetaObject::Connection, 3> m_connections;
};
} // namespace AetherSDR
