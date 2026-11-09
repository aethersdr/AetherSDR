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
class NativeOperatingDisplayWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativeOperatingDisplayWidget(RadioModel& model,QWidget* parent=nullptr);
    ~NativeOperatingDisplayWidget() override;
private:
    void bindBackend();
    void acceptStatus(const QVariantMap& status);
    void render();
    void refreshAvailability();
    void editDraft();
    void read();
    void submit(const QString& verb,const QVariantMap& arguments={});
    bool available(const QString& operation) const;
    bool applyAvailable() const;
    QVariantMap draftChoice() const;
    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QVariantMap m_confirmed;
    std::array<QMetaObject::Connection,3> m_connections;
    QMap<quint64,QString> m_requests;
    quint64 m_bindingGeneration=0,m_receiveSession=0,m_statusRevision=0,m_statusSerial=0,m_getSerial=0;
    bool m_readAfterGet=false,m_seeded=false,m_dirty=false,m_retired=false;
    int m_draftValue=-1,m_baseline=-1;
    QString m_draftContext,m_draftRevision;
    QComboBox* m_draft;
    QPushButton* m_read;
    QPushButton* m_cancel;
    QPushButton* m_apply;
    QPushButton* m_escape;
    QLabel* m_basic;
    QLabel* m_xv;
    QLabel* m_screen;
    QLabel* m_edit;
    QLabel* m_restrictions;
    QLabel* m_progress;
    QLabel* m_write;
    QLabel* m_status;
};
} // namespace AetherSDR
