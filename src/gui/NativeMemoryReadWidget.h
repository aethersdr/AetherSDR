#pragma once

#include <QMap>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QWidget>
#include <array>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
namespace AetherSDR {
class ControlAvailabilityRegistry;
class IRadioBackend;
class RadioModel;

class NativeMemoryReadWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativeMemoryReadWidget(RadioModel& model, QWidget* parent = nullptr);
    ~NativeMemoryReadWidget() override;
private:
    void bindBackend();
    void acceptStatus(const QVariantMap& status);
    void render();
    void inspect(bool quick);
    void submit(const QString& verb, const QVariantMap& arguments = {});
    void refreshAvailability();
    bool canRead() const;
    bool canCancel() const;

    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QVariantMap m_confirmed;
    std::array<QMetaObject::Connection, 3> m_connections;
    QMap<quint64, QString> m_requests;
    quint64 m_bindingGeneration = 0;
    quint64 m_receiveSession = 0;
    quint64 m_statusRevision = 0;
    quint64 m_statusSerial = 0;
    quint64 m_getSerial = 0;
    bool m_refreshAfterGet = false;
    bool m_inspectQuick = false;
    QTableWidget* m_channels;
    QTableWidget* m_quick;
    QSpinBox* m_channelDraft;
    QSpinBox* m_quickDraft;
    QLabel* m_metadata;
    QLabel* m_progress;
    QLabel* m_status;
    QPlainTextEdit* m_details;
    QPushButton* m_readAll;
    QPushButton* m_readChannel;
    QPushButton* m_readQuick;
    QPushButton* m_refresh;
    QPushButton* m_cancel;
};
} // namespace AetherSDR
