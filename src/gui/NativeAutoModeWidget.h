#pragma once

#include <QMap>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QWidget>
#include <array>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;
namespace AetherSDR {
class ControlAvailabilityRegistry;
class IRadioBackend;
class RadioModel;
class NativeAutoModeWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativeAutoModeWidget(RadioModel& model,QWidget* parent=nullptr);
    ~NativeAutoModeWidget() override;
private:
    void bindBackend();
    void acceptStatus(const QVariantMap& status);
    void render();
    void refreshAvailability();
    void editEnabled();
    void editDivision();
    void selectDivision(int index);
    void submit(const QString& verb,const QVariantMap& arguments={});
    bool available(const QString& operation) const;
    bool enabledAvailable() const;
    bool registerAvailable() const;
    bool deleteAvailable() const;
    QVariantMap selectedRow() const;
    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QVariantMap m_confirmed;
    std::array<QMetaObject::Connection,3> m_connections;
    QMap<quint64,QString> m_requests;
    quint64 m_bindingGeneration=0,m_receiveSession=0,m_statusRevision=0,m_statusSerial=0,m_getSerial=0;
    bool m_enabledSeeded=false,m_enabledDirty=false,m_enabledRetired=false,m_enabledBaseline=false;
    QString m_modeDraft;
    QString m_enabledContext,m_enabledRevision,m_divisionContext,m_divisionSnapshot,m_deleteContext,m_deleteSnapshot,m_deleteRevision;
    int m_selectedIndex=-1;
    QCheckBox* m_enabled;
    QComboBox* m_mode;
    QDoubleSpinBox* m_frequency;
    QTableWidget* m_rows;
    QLabel* m_observed;
    QLabel* m_status;
    QLabel* m_progress;
    QLabel* m_writeStatus;
    QPushButton* m_read;
    QPushButton* m_cancel;
    QPushButton* m_enabledApply;
    QPushButton* m_register;
    QPushButton* m_delete;
};
} // namespace AetherSDR
