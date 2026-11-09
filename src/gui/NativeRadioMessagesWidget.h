#pragma once

#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QWidget>
#include <array>

class QLabel;
class QPushButton;
class QSpinBox;

namespace AetherSDR {
class ControlAvailabilityRegistry;
class IRadioBackend;
class RadioModel;

class NativeRadioMessagesWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativeRadioMessagesWidget(RadioModel& model, QWidget* parent = nullptr);
    ~NativeRadioMessagesWidget() override;

private:
    void bindBackend();
    void acceptStatus(const QVariantMap& status);
    void render();
    void refreshAvailability();
    void editDraft();
    void submit(bool read, bool reset = false);
    bool writable() const;

    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QVariantMap m_confirmed;
    std::array<QMetaObject::Connection, 3> m_connections;
    quint64 m_bindingGeneration = 0;
    quint64 m_receiveSession = 0;
    quint64 m_statusRevision = 0;
    quint64 m_statusSerial = 0;
    quint64 m_getSerial = 0;
    quint64 m_request = 0;
    bool m_readRequest = false;
    bool m_seeded = false;
    bool m_dirty = false;
    bool m_draftRetired = false;
    int m_draftBaseline = -1;
    QString m_draftContext;
    QString m_draftRevision;
    std::array<QLabel*, 2> m_messages;
    QLabel* m_priority;
    QLabel* m_observed;
    QLabel* m_status;
    QSpinBox* m_draft;
    QPushButton* m_apply;
    QPushButton* m_read;
    QPushButton* m_reset;
};
} // namespace AetherSDR
