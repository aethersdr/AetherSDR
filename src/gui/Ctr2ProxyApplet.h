#pragma once

#include <QPointer>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class GuardedComboBox;

namespace AetherSDR {

class Ctr2ProxyModel;

// CTR2 Proxy applet: Wi-Fi or USB mode, explicit endpoints, Start/Stop, and the
// relay's state, endpoints, byte counters and last local error. All behavior
// lives in Ctr2ProxyModel; hiding the applet does not stop the proxy.
class Ctr2ProxyApplet : public QWidget {
    Q_OBJECT

public:
    explicit Ctr2ProxyApplet(QWidget* parent = nullptr);

    void setModel(Ctr2ProxyModel* model);

private:
    void buildUi();
    void syncAddresses();
    void syncConfiguration();
    void syncStatus();
    void syncStats();

    QPointer<Ctr2ProxyModel> m_model;
    GuardedComboBox* m_modeCombo{nullptr};
    GuardedComboBox* m_listenCombo{nullptr};
    GuardedComboBox* m_usbCombo{nullptr};
    QPushButton* m_refreshBtn{nullptr};
    QLineEdit* m_listenPortEdit{nullptr};
    QLabel* m_radioLabel{nullptr};
    QPushButton* m_startBtn{nullptr};
    QLabel* m_stateLabel{nullptr};
    QLabel* m_endpointsLabel{nullptr};
    QLabel* m_trafficLabel{nullptr};
    QLabel* m_problemLabel{nullptr};
    QLabel* m_errorLabel{nullptr};
};

} // namespace AetherSDR
