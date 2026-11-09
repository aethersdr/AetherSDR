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
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
namespace AetherSDR {
class ControlAvailabilityRegistry;
class IRadioBackend;
class RadioModel;
class NativeCwReceiveWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativeCwReceiveWidget(RadioModel& model,QWidget* parent=nullptr);
    ~NativeCwReceiveWidget() override;
private:
    struct Row {
        QLabel* observed=nullptr;
        QComboBox* choice=nullptr;
        QSpinBox* number=nullptr;
        QPushButton* apply=nullptr;
        bool seeded=false,dirty=false,retired=false;
        int draft=0,baseline=0;
        QString context,revision;
    };
    void bindBackend();
    void acceptStatus(const QVariantMap& status);
    void acceptText(const QVariantMap& status);
    void mergeText(const QVariantMap& stream,const QVariantMap& transcript);
    void render();
    void renderText();
    void refreshAvailability();
    void editDraft(const QString& id);
    void read();
    void submit(const QString& verb,const QVariantMap& arguments={});
    QVariantMap control(const QString& id) const;
    bool rowAvailable(const QString& id,bool apply=false) const;
    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QVariantMap m_confirmed,m_streamState,m_transcriptState;
    QMap<QString,Row> m_rows;
    std::array<QMetaObject::Connection,3> m_connections;
    QMap<quint64,QString> m_requests;
    quint64 m_bindingGeneration=0,m_receiveSession=0,m_statusRevision=0,m_textRevision=0,m_statusSerial=0,m_textSerial=0;
    bool m_readAfterGet=false,m_boundConnected=false;
    QPushButton* m_read;
    QPushButton* m_cancel;
    QPushButton* m_clear;
    QLabel* m_autoZero;
    QLabel* m_stream;
    QLabel* m_progress;
    QLabel* m_write;
    QLabel* m_status;
    QLabel* m_restrictions;
    QPlainTextEdit* m_transcript;
};
} // namespace AetherSDR
