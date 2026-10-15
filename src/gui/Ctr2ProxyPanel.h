#pragma once

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class GuardedComboBox;

namespace AetherSDR {

class Ctr2ProxyModel;

// The CTR2 relay's controls, at the top of the CTR2 window
// (FirmwareInstallDialog): Wi-Fi or USB mode, explicit endpoints,
// Start/Stop, and the relay's state, endpoints, byte counters and last local
// error. Styled by its host's canon sheet: row labels are SectionLabel, and
// values carry a "tone" property (secondary, warning, danger). All behavior
// lives in Ctr2ProxyModel; closing the window does not stop the proxy.
class Ctr2ProxyPanel : public QWidget {
    Q_OBJECT

public:
    explicit Ctr2ProxyPanel(QWidget* parent = nullptr);

    void setModel(Ctr2ProxyModel* model);

private:
    void buildUi();
    void syncAddresses();
    void syncConfiguration();
    void syncStatus();
    void syncStats();
    void onStartClicked();
#ifdef Q_OS_LINUX
    void offerUsbAccessRule();
    void showManualRuleInstructions(const QString& why);
    void installUsbAccessRule();
    void startAfterRuleInstalled(int attemptsLeft);
#endif

    QPointer<Ctr2ProxyModel> m_model;
    GuardedComboBox* m_modeCombo{nullptr};
    GuardedComboBox* m_listenCombo{nullptr};
    GuardedComboBox* m_usbCombo{nullptr};
    QLabel* m_listenRowLabel{nullptr};
    QLabel* m_usbRowLabel{nullptr};
    QPushButton* m_refreshBtn{nullptr};
    QLineEdit* m_listenPortEdit{nullptr};
    QLabel* m_radioLabel{nullptr};
    QPushButton* m_startBtn{nullptr};
    QLabel* m_stateLabel{nullptr};
    QLabel* m_endpointsLabel{nullptr};
    // Traffic readout: a fixed table, so changing digits never move it.
    QWidget* m_traffic{nullptr};
    QLabel* m_bytesUp{nullptr};
    QLabel* m_bytesDown{nullptr};
    QLabel* m_queuedUp{nullptr};
    QLabel* m_queuedDown{nullptr};
    QLabel* m_udpName{nullptr};
    QLabel* m_udpUp{nullptr};
    QLabel* m_udpDown{nullptr};
    QLabel* m_extraName{nullptr};   // Dropped (USB) or Rejected (Wi-Fi)
    QLabel* m_extraValue{nullptr};
    QCheckBox* m_autoStart{nullptr};
    QLabel* m_problemLabel{nullptr};
    QLabel* m_errorLabel{nullptr};
    bool m_installingRule{false};  // pkexec rule install in flight
};

} // namespace AetherSDR
