#pragma once

#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QWidget>

#include <array>

class QLabel;
class QSpinBox;

namespace AetherSDR {
class ControlAvailabilityRegistry;
class IRadioBackend;
class RadioModel;

class NetworkAudioLevelsWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NetworkAudioLevelsWidget(RadioModel& model, QWidget* parent = nullptr);

private:
    void bindBackend();
    void acceptStatus(const QVariantMap& status);
    void render();
    void submit(int direction);

    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QVariantMap m_confirmed;
    std::array<QSpinBox*, 2> m_levels;
    std::array<bool, 2> m_seeded{false, false};
    QLabel* m_readback;
    QLabel* m_status;
    quint64 m_query = 0;
    quint64 m_edit = 0;
    quint64 m_bindingGeneration = 0;
    quint64 m_receiveSession = 0;
    quint64 m_statusSerial = 0;
    quint64 m_getSerial = 0;
    quint64 m_statusRevision = 0;
    std::array<QMetaObject::Connection, 3> m_connections;
};
} // namespace AetherSDR
