#pragma once

#include <QMap>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QWidget>

#include <array>

class QComboBox;
class QLabel;
class QPushButton;

namespace AetherSDR {
class ControlAvailabilityRegistry;
class IRadioBackend;
class RadioModel;

class NativeReceiveSettingsWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativeReceiveSettingsWidget(RadioModel& model, QWidget* parent = nullptr);

private:
    void bindBackend();
    void acceptStatus(const QVariantMap& status);
    void readSettings();
    void render();
    void submit(const QString& id, int index, bool reset = false);
    QVariantMap control(const QString& id) const;

    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QVariantMap m_confirmed;
    QMap<QString, QComboBox*> m_editors;
    QMap<QString, QPushButton*> m_resetButtons;
    QLabel* m_context;
    QLabel* m_status;
    quint64 m_bindingGeneration = 0;
    quint64 m_receiveSession = 0;
    quint64 m_statusSerial = 0;
    quint64 m_getSerial = 0;
    quint64 m_statusRevision = 0;
    quint64 m_query = 0;
    QMap<quint64, QString> m_requests;
    std::array<QMetaObject::Connection, 3> m_connections;
};
} // namespace AetherSDR
