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
class QLabel;
class QPushButton;
class QSpinBox;

namespace AetherSDR {
class ControlAvailabilityRegistry;
class IRadioBackend;
class RadioModel;

class NativeReceiveEqWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativeReceiveEqWidget(RadioModel& model, QWidget* parent = nullptr);

private:
    void bindBackend();
    void acceptStatus(const QVariantMap& status);
    void render();
    void submit(const QString& verb, const QVariantMap& arguments = {});
    bool operationAvailable(const QString& operation) const;
    void applyCurve();
    void resetBands(bool all);
    void selectCopyDestination();

    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QVariantMap m_confirmed;
    QString m_draftContext;
    QString m_draftRevision;
    QString m_inspectionToken;
    bool m_draftDirty = false;
    quint64 m_bindingGeneration = 0;
    quint64 m_receiveSession = 0;
    quint64 m_statusSerial = 0;
    quint64 m_getSerial = 0;
    quint64 m_statusRevision = 0;
    QMap<quint64, QString> m_requests;
    QMetaObject::Connection m_statusConnection;
    QMetaObject::Connection m_resultConnection;
    QMetaObject::Connection m_errorConnection;
    QCheckBox* m_enabled;
    QComboBox* m_effect;
    std::array<QLabel*, 18> m_observed{};
    std::array<QSpinBox*, 18> m_draft{};
    std::array<QCheckBox*, 18> m_resetSelection{};
    QPushButton* m_read;
    QPushButton* m_apply;
    QPushButton* m_resetSelected;
    QPushButton* m_resetAll;
    QComboBox* m_destination;
    QPushButton* m_copy;
    QPushButton* m_selectDestination;
    QPushButton* m_inspect;
    QLabel* m_context;
    QLabel* m_status;
};
} // namespace AetherSDR
