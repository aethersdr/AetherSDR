#pragma once

#include <QMap>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QWidget>
#include <array>

class QCheckBox;
class QLabel;
class QPushButton;

namespace AetherSDR {
class ControlAvailabilityRegistry;
class IRadioBackend;
class RadioModel;

class NativeReceiveRoutingWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativeReceiveRoutingWidget(RadioModel& model, QWidget* parent = nullptr);

private:
    void bindBackend();
    void acceptStatus(const QVariantMap& status);
    void render();
    bool available(const QString& operation) const;
    void submit(const QString& verb, const QString& operation, const QVariantMap& arguments = {});

    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QVariantMap m_confirmed;
    quint64 m_bindingGeneration = 0;
    quint64 m_receiveSession = 0;
    quint64 m_statusSerial = 0;
    quint64 m_getSerial = 0;
    quint64 m_statusRevision = 0;
    QMap<quint64, QString> m_requests;
    QMetaObject::Connection m_statusConnection;
    QMetaObject::Connection m_resultConnection;
    QMetaObject::Connection m_errorConnection;
    std::array<QCheckBox*, 2> m_controls{};
    QPushButton* m_read;
    QLabel* m_context;
    QLabel* m_status;
};
} // namespace AetherSDR
