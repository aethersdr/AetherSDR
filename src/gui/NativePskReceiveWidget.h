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
class NativePskReceiveWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativePskReceiveWidget(RadioModel& model,QWidget* parent=nullptr);
    ~NativePskReceiveWidget() override;
private:
    struct Row { QLabel* observed=nullptr; QComboBox* draft=nullptr; QPushButton* apply=nullptr; QPushButton* reset=nullptr; bool seeded=false,dirty=false,retired=false; int value=-1,baseline=-1; QString context,revision; };
    void bindBackend();
    void acceptStatus(const QVariantMap& status);
    void render();
    void refreshAvailability();
    void editDraft(const QString& id, int index);
    void read();
    void submit(const QString& verb,const QVariantMap& arguments={});
    QVariantMap descriptor(const QString& id) const;
    bool available(const QString& id,bool apply=false,bool reset=false) const;
    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QVariantMap m_confirmed;
    QMap<QString,Row> m_rows;
    std::array<QMetaObject::Connection,3> m_connections;
    QMap<quint64,QString> m_requests;
    quint64 m_bindingGeneration=0,m_statusRevision=0,m_statusSerial=0,m_getSerial=0;
    bool m_readAfterGet=false;
    QPushButton* m_read;
    QPushButton* m_cancel;
    QLabel* m_bank;
    QLabel* m_notice;
    QLabel* m_progress;
    QLabel* m_write;
    QLabel* m_status;
};
} // namespace AetherSDR
