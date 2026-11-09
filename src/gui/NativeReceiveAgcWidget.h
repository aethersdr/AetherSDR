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
class QSpinBox;

namespace AetherSDR {
class ControlAvailabilityRegistry;
class IRadioBackend;
class RadioModel;

class NativeReceiveAgcWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativeReceiveAgcWidget(RadioModel& model, QWidget* parent = nullptr);

private:
    void bindBackend();
    void acceptStatus(const QVariantMap& status);
    void render();
    bool available(const QString& operation) const;
    void submit(const QString& verb, const QString& operation, const QVariantMap& arguments = {});
    void changePreset(int bank, bool reset);
    void changeThreshold(bool reset);

    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QVariantMap m_confirmed;
    QString m_draftContext;
    QString m_draftRevision;
    std::array<bool, 3> m_dirty{};
    bool m_thresholdDirty = false;
    quint64 m_bindingGeneration = 0;
    quint64 m_receiveSession = 0;
    quint64 m_statusSerial = 0;
    quint64 m_getSerial = 0;
    quint64 m_statusRevision = 0;
    QMap<quint64, QString> m_requests;
    QMetaObject::Connection m_statusConnection;
    QMetaObject::Connection m_resultConnection;
    QMetaObject::Connection m_errorConnection;
    std::array<QLabel*, 3> m_observed{};
    std::array<QSpinBox*, 3> m_draft{};
    std::array<QPushButton*, 3> m_apply{};
    std::array<QPushButton*, 3> m_reset{};
    QCheckBox* m_quick;
    QLabel* m_thresholdObserved;
    QSpinBox* m_threshold;
    QPushButton* m_thresholdApply;
    QPushButton* m_thresholdReset;
    QPushButton* m_restore;
    QPushButton* m_read;
    QLabel* m_context;
    QLabel* m_status;
};
} // namespace AetherSDR
