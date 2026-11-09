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
class QSpinBox;
class QVBoxLayout;

namespace AetherSDR {
class ControlAvailabilityRegistry;
class IRadioBackend;
class RadioModel;

// Client of the versioned vfo extension contract. A/B are tuning memories,
// independent of the number of simultaneous receive slices.
class NativeVfoSettingsWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativeVfoSettingsWidget(RadioModel& model, QWidget* parent = nullptr);

private:
    void bindBackend();
    void acceptStatus(const QVariantMap& status);
    void submit(const QString& verb, const QVariant& value);
    void render();
    void acceptTuningStatus(const QVariantMap& status);
    void queryTuning();
    void submitTuning(const QString& field, bool value);
    void renderTuning();

    void setupRelative(QVBoxLayout* layout);
    void bindRelative();
    void acceptRelativeStatus(const QVariantMap& status);
    void renderRelative();
    void invokeRelative(const QString& verb);
    QPointer<IRadioBackend> m_relativeBackend;
    QString m_relativeNamespace;
    QVariantMap m_relativeConfirmed;
    quint64 m_relativeGeneration = 0;
    quint64 m_relativeSession = 0;
    quint64 m_relativeRevision = 0;
    quint64 m_relativeRender = 0;
    quint64 m_relativeRequest = 0;
    QString m_relativeDraft;
    QComboBox* m_relativeFcDirection = nullptr;
    QComboBox* m_relativeMultiplier = nullptr;
    QComboBox* m_relativeUdDirection = nullptr;
    QSpinBox* m_relativeSteps = nullptr;
    QCheckBox* m_relativeSingle = nullptr;
    QPushButton* m_relativeRead = nullptr;
    QPushButton* m_relativeCancel = nullptr;
    QPushButton* m_relativeFc = nullptr;
    QPushButton* m_relativeUd = nullptr;
    QLabel* m_relativeObserved = nullptr;
    QLabel* m_relativeReason = nullptr;
    std::array<QMetaObject::Connection, 3> m_relativeConnections;

    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QString m_tuningNamespace;
    QVariantMap m_tuningConfirmed;
    std::array<QCheckBox*, 3> m_tuning;
    QLabel* m_tuningReason;
    QPushButton* m_tuningRead;
    quint64 m_tuningQuery = 0;
    quint64 m_tuningStatusRevision = 0;
    quint64 m_tuningStatusSerial = 0;
    quint64 m_tuningGetSerial = 0;
    QMap<quint64, QString> m_tuningEdits;
    QVariantMap m_confirmed;
    QComboBox* m_receive;
    QComboBox* m_transmit;
    std::array<QDoubleSpinBox*, 2> m_frequency;
    std::array<bool, 2> m_seeded{false, false};
    QLabel* m_readback;
    QLabel* m_status;
    quint64 m_bindingGeneration = 0;
    quint64 m_receiveSession = 0;
    quint64 m_query = 0;
    quint64 m_edit = 0;
    quint64 m_vfoStatusRevision = 0;
    quint64 m_vfoStatusSerial = 0;
    quint64 m_vfoGetSerial = 0;
    QMetaObject::Connection m_statusConnection;
    QMetaObject::Connection m_resultConnection;
    QMetaObject::Connection m_errorConnection;
};
} // namespace AetherSDR
