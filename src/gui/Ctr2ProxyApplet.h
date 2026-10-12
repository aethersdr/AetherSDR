#pragma once

#include <QPointer>
#include <QWidget>

#include <vector>

class QCheckBox;
class QLabel;
class QVBoxLayout;
class QLineEdit;
class QPushButton;
class GuardedComboBox;

namespace AetherSDR {

class Ctr2ProxyModel;

// CTR2 Proxy applet: a Wi-Fi listener and a checklist of USB devices, relayed
// together by one Start/Stop, with a status row per link (state, byte
// counters, last local error). All behavior lives in Ctr2ProxyModel; hiding
// the applet does not stop the proxy.
class Ctr2ProxyApplet : public QWidget {
    Q_OBJECT

public:
    explicit Ctr2ProxyApplet(QWidget* parent = nullptr);

    void setModel(Ctr2ProxyModel* model);

private:
    struct LinkRow {
        QWidget* row{nullptr};
        QLabel* title{nullptr};
        QLabel* traffic{nullptr};
        QLabel* error{nullptr};
    };

    void buildUi();
    LinkRow& linkRow(int index);
    void syncAddresses();
    void syncConfiguration();
    void syncStatus();
    void onStartClicked();
#ifdef Q_OS_LINUX
    void offerUsbAccessRule();
    void showManualRuleInstructions(const QString& why);
    void installUsbAccessRule();
    void startAfterRuleInstalled(int attemptsLeft);
#endif

    QPointer<Ctr2ProxyModel> m_model;
    QCheckBox* m_wifiCheck{nullptr};
    GuardedComboBox* m_listenCombo{nullptr};
    QWidget* m_usbList{nullptr};
    QVBoxLayout* m_usbListLayout{nullptr};
    QLabel* m_usbEmptyLabel{nullptr};
    std::vector<QCheckBox*> m_usbChecks;
    QPushButton* m_refreshBtn{nullptr};
    QLineEdit* m_listenPortEdit{nullptr};
    QLabel* m_radioLabel{nullptr};
    QPushButton* m_startBtn{nullptr};
    QLabel* m_stateLabel{nullptr};
    QWidget* m_linksPane{nullptr};
    QVBoxLayout* m_linksLayout{nullptr};
    std::vector<LinkRow> m_linkRows;
    QLabel* m_problemLabel{nullptr};
    QLabel* m_errorLabel{nullptr};
    bool m_installingRule{false};  // pkexec rule install in flight
};

} // namespace AetherSDR
