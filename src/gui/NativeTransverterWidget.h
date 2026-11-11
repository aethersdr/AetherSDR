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
class QLineEdit;
class QPushButton;
class QTableWidget;
namespace AetherSDR {
class ControlAvailabilityRegistry;
class IRadioBackend;
class RadioModel;
class NativeTransverterWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativeTransverterWidget(RadioModel& model,QWidget* parent=nullptr);
    ~NativeTransverterWidget() override;
private:
    struct Request { QString verb,context,frequencyRevision; quint64 draftSerial=0; int direction=0; qint64 magnitude=0; };
    void bindBackend();
    void acceptStatus(const QVariantMap& status);
    void render();
    void refreshAvailability();
    void editEnabled();
    void editOffset();
    void read();
    void preview();
    void submit(const QString& verb,const QVariantMap& arguments={});
    bool available(const QString& operation) const;
    bool enabledAvailable() const;
    bool offsetAvailable() const;
    bool offsetValue(int& direction,qint64& magnitude) const;
    bool busy() const;
    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QVariantMap m_confirmed,m_preview;
    std::array<QMetaObject::Connection,3> m_connections;
    QMap<quint64,Request> m_requests;
    quint64 m_bindingGeneration=0,m_receiveSession=0,m_statusRevision=0,m_statusSerial=0,m_draftSerial=0,m_previewRequest=0;
    bool m_readAfterGet=false,m_enabledSeeded=false,m_enabledDirty=false,m_enabledRetired=false,m_enabledDraft=false,m_enabledBaseline=false;
    bool m_offsetSeeded=false,m_offsetDirty=false,m_offsetRetired=false;
    int m_directionBaseline=0,m_directionDraft=0;
    qint64 m_magnitudeBaseline=0;
    QString m_enabledContext,m_enabledRevision,m_offsetContext,m_offsetRevision,m_frequencyRevision;
    QComboBox* m_enabled;
    QComboBox* m_direction;
    QLineEdit* m_magnitude;
    QComboBox* m_scope;
    QPushButton* m_read;
    QPushButton* m_cancel;
    QPushButton* m_applyEnabled;
    QPushButton* m_applyOffset;
    QPushButton* m_previewButton;
    QLabel* m_observed;
    QLabel* m_bounds;
    QLabel* m_previewLabel;
    QLabel* m_if;
    QLabel* m_notice;
    QLabel* m_progress;
    QLabel* m_write;
    QLabel* m_status;
    QTableWidget* m_frequencies;
};
} // namespace AetherSDR
