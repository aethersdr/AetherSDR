#pragma once

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

class NativeFilterSettingsWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativeFilterSettingsWidget(RadioModel& model, QWidget* parent = nullptr);

private:
    void bindBackend();
    void acceptFmStatus(const QVariantMap& status);
    void renderFm();
    void invokeFm(const QString& verb);
    void acceptStatus(const QVariantMap& status);
    void render();
    void submit(int axis, int index);
    void acceptBankStatus(const QVariantMap& status);
    void renderBanks();
    void queryBanks();
    void submitBank(int row, int choice, bool reset);

    QString m_fmNamespace;
    QVariantMap m_fmConfirmed;
    std::array<QLabel*, 2> m_fmObserved;
    QComboBox* m_fmChoice;
    QPushButton* m_fmRead;
    QPushButton* m_fmApply;
    QPushButton* m_fmCancel;
    QLabel* m_fmStatus;
    quint64 m_fmRevision = 0;
    quint64 m_fmRenderSerial = 0;
    quint64 m_fmRequest = 0;
    bool m_fmDirty = false;
    QString m_fmDraftRevision;
    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QString m_bankNamespace;
    QVariantMap m_bankConfirmed;
    std::array<QComboBox*, 10> m_banks;
    std::array<QPushButton*, 9> m_bankResets;
    std::array<QLabel*, 3> m_bandwidths;
    QLabel* m_bankInfo;
    QLabel* m_bankStatus;
    QPushButton* m_bankRefresh;
    quint64 m_bankQuery = 0;
    quint64 m_bankEdit = 0;
    quint64 m_bindingGeneration = 0;
    quint64 m_receiveSession = 0;
    quint64 m_filterRevision = 0;
    quint64 m_bankRevision = 0;
    quint64 m_filterSerial = 0;
    quint64 m_bankSerial = 0;
    quint64 m_filterGetSerial = 0;
    quint64 m_bankGetSerial = 0;
    QVariantMap m_confirmed;
    std::array<QComboBox*, 2> m_axes;
    std::array<QLabel*, 2> m_labels;
    QLabel* m_mode;
    QLabel* m_status;
    quint64 m_query = 0;
    quint64 m_edit = 0;
    std::array<QMetaObject::Connection, 3> m_connections;
};
} // namespace AetherSDR
