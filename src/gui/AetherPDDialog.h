#pragma once

#include "CanonWindow.h"
#include "models/RadioModel.h"

#include <QPointer>
#include <functional>
#include <optional>

class QLabel;
class QComboBox;
class QPushButton;
class QPlainTextEdit;
class QTimer;

namespace AetherSDR {
class AetherPDMeasurementDialog;

// Observes the current radio's PD service; calibration arm never keys RF.
class AetherPDDialog : public CanonWindow {
    Q_OBJECT
public:
    using TokenLoader = std::function<void(const QString&, QObject*,
                                           std::function<void(const QString&)>)>;
    explicit AetherPDDialog(RadioModel* radio, QWidget* parent = nullptr);
    AetherPDDialog(RadioModel* radio, TokenLoader loadToken, QWidget* parent = nullptr);

public slots:
    void done(int result) override;

private:
    void refresh();
    void render();
    void watchPan(PanadapterModel* pan);
    void renderIqChoices();
    std::optional<aetherpd::IQSelection> iqSelection() const;
    void renderInstallation();
    void prepareInstallation();
    void prepareFirstInstallation();
    void checkInstallation();
    void prepareSavedInstallation();
    void cleanupInstallation();
    void approveInstallation();
    void cancelInstallation();
    void request(aetherpd::Operation operation);
    bool current() const;

    QPointer<AetherPDMeasurementDialog> m_measurement;
    QPointer<RadioModel> m_radio;
    RadioControlSession m_session;
    QString m_serial;
    TokenLoader m_loadToken;
    quint64 m_loadGeneration{0};
    bool m_loading{false};
    bool m_closed{false};
    QString m_notice;
    QByteArray m_displayedPlan;
    QLabel* m_installState{nullptr};
    QPlainTextEdit* m_installPlan{nullptr};
    QPushButton* m_installPrepare{nullptr};
    QPushButton* m_installFirst{nullptr};
    QPushButton* m_installCheck{nullptr};
    QPushButton* m_installSaved{nullptr};
    QPushButton* m_installCleanup{nullptr};
    QPushButton* m_installApprove{nullptr};
    QPushButton* m_installCancel{nullptr};
    QTimer* m_installRefresh{nullptr};
    QComboBox* m_iqChoice{nullptr};
    QLabel* m_iqObserved{nullptr};
    QLabel* m_state{nullptr};
    QLabel* m_detail{nullptr};
    QLabel* m_receipt{nullptr};
    QLabel* m_message{nullptr};
    QPushButton* m_measurementOpen{nullptr};
    QPushButton* m_refresh{nullptr};
    QPushButton* m_start{nullptr};
    QPushButton* m_enableCorrection{nullptr};
    QPushButton* m_bypassCorrection{nullptr};
    QPushButton* m_arm{nullptr};
    QPushButton* m_cancel{nullptr};
    QPushButton* m_stop{nullptr};
    QPushButton* m_restore{nullptr};
};

} // namespace AetherSDR
