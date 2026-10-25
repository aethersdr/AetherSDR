#pragma once

#include <QPointer>
#include <QWidget>

#include <functional>

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

#ifdef Q_OS_LINUX
    // What the CTR2 access rule is wanted for: the relay's Start, a firmware
    // action on a CTR2 running its firmware (HID), or one on a CTR2-Max in
    // download mode (its serial port). One rule covers all three; the
    // questions differ.
    enum class RuleUse { Relay, Firmware, DownloadMode };
    // Whether this build's rule is already installed, locally or packaged.
    static bool accessRuleInstalled();
    // A CTR2 window action found the device closed to this login: offer the
    // rule, and call afterInstalled once it is in place. False when no install
    // started (declined, already installed, no pkexec, one already running):
    // the caller carries on as it would without the rule.
    bool offerAccessRule(RuleUse use, std::function<void()> afterInstalled);
#endif

private:
    void buildUi();
    void syncAddresses();
    void syncConfiguration();
    void syncStatus();
    void syncStats();
    void onStartClicked();
#ifdef Q_OS_LINUX
    // True when the install started.
    bool offerUsbAccessRule(RuleUse use = RuleUse::Relay);
    void showManualRuleInstructions(const QString& why);
    void installUsbAccessRule(RuleUse use);
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
    std::function<void()> m_afterRule;   // a CTR2 window action's next step
};

} // namespace AetherSDR
