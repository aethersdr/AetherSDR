#pragma once

#include "core/RadioDiscovery.h"
#include "core/SmartLinkClient.h"

#include <QWidget>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QButtonGroup>
#include <QCommandLinkButton>
#include <QStackedWidget>
#include <QToolButton>

class QVBoxLayout;

namespace AetherSDR {

// Novice-first dialog for local, SmartLink, and manual/VPN radio connections.
class ConnectionPanel : public QWidget {
    Q_OBJECT

public:
    explicit ConnectionPanel(QWidget* parent = nullptr);

    void setFramelessMode(bool on);
    void setConnected(bool connected);
    void setStatusText(const QString& text);
    void probeRadio(const QString& ip);

protected:
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* e) override;

public slots:
    void onRadioDiscovered(const RadioInfo& radio);
    void onRadioUpdated(const RadioInfo& radio);
    void onRadioLost(const QString& serial);

    // SmartLink
    void setSmartLinkClient(SmartLinkClient* client);

signals:
    void connectRequested(const RadioInfo& radio);
    void wanConnectRequested(const WanRadioInfo& radio);
    void wanDisconnectClientsRequested(const WanRadioInfo& radio);
    void disconnectRequested();
    void routedRadioFound(const RadioInfo& radio);
    void retryDiscoveryRequested();
    void networkDiagnosticsRequested();
    void smartLinkLoginRequested(const QString& email, const QString& password);

private slots:
    void onConnectionModeClicked(int id);
    void onListSelectionChanged();
    void onWanSelectionChanged();
    void onLocalConnectClicked();
    void onWanConnectClicked();
    void onWanDisconnectClientsClicked();
    void onManualIpChanged(const QString& ip);
    void onManualConnectClicked();
    void onManualAdvancedToggled(bool checked);

private:
    enum ConnectionMode {
        LocalMode = 0,
        SmartLinkMode = 1,
        ManualMode = 2
    };

    void setCurrentMode(ConnectionMode mode);
    void updateLocalPageState();
    void updateSmartLinkUi();
    void updateActionState();
    void updateLowBandwidthVisibility();
    void updateManualAdvancedVisibility();
    void refreshManualSourceOptions(const RadioBindSettings* selected = nullptr);
    void applySavedSourceSelection(const QString& ip);
    RadioBindSettings currentManualBindSettings(bool* staleSelection = nullptr) const;
    void loadRecentManualIps();
    void rememberManualIp(const QString& ip);
    void saveManualProfile(const QString& targetIp,
                           const RadioBindSettings& settings,
                           const QHostAddress& lastSuccessfulLocalIp);
    void saveLowBandwidthPreference(bool enabled);
    void setManualMessage(const QString& text, bool error = false);
    QString formatLocalRadioLabel(const RadioInfo& radio) const;
    QString formatWanRadioLabel(const WanRadioInfo& radio) const;

    QWidget*     m_titleBar{nullptr};
    QVBoxLayout* m_rootLayout{nullptr};

    QButtonGroup* m_modeButtons{nullptr};
    QStackedWidget* m_modeStack{nullptr};
    QCommandLinkButton* m_localModeBtn{nullptr};
    QCommandLinkButton* m_smartLinkModeBtn{nullptr};
    QCommandLinkButton* m_manualModeBtn{nullptr};

    QLabel*      m_statusLabel;
    QPushButton* m_disconnectBtn{nullptr};

    QListWidget* m_radioList{nullptr};
    QStackedWidget* m_localStateStack{nullptr};
    QWidget* m_localEmptyState{nullptr};
    QPushButton* m_localConnectBtn{nullptr};

    QList<RadioInfo> m_radios;   // LAN radios only
    bool m_connected{false};

    // SmartLink UI
    SmartLinkClient* m_smartLink{nullptr};
    QWidget*     m_loginForm{nullptr};
    QLineEdit*   m_emailEdit{nullptr};
    QLineEdit*   m_passwordEdit{nullptr};
    QPushButton* m_loginBtn{nullptr};
    QPushButton* m_logoutBtn{nullptr};
    QLabel*      m_slUserLabel{nullptr};
    QListWidget* m_wanList{nullptr};
    QLabel*      m_smartLinkEmptyLabel{nullptr};
    QPushButton* m_wanDisconnectClientsBtn{nullptr};
    QPushButton* m_wanConnectBtn{nullptr};
    QList<WanRadioInfo> m_wanRadios;

    // Manual (VPN / routed) connection
    QComboBox*   m_manualIpCombo{nullptr};
    QLineEdit*   m_manualIpEdit{nullptr};
    QLabel*      m_manualResultLabel{nullptr};
    QToolButton* m_manualAdvancedToggle{nullptr};
    QWidget*     m_manualAdvancedWidget{nullptr};
    QComboBox*   m_manualSourceCombo{nullptr};
    QLabel*      m_manualSourceWarningLabel{nullptr};
    QPushButton* m_manualConnectBtn{nullptr};
    QString      m_manualProfileIp;
    bool         m_manualConnectPending{false};

    QCheckBox*   m_autoConnectCheck{nullptr};

    QWidget*     m_linkOptionsWidget{nullptr};
    QLabel*      m_lowBwHintLabel{nullptr};
    QCheckBox*   m_lowBwCheck{nullptr};
    QCheckBox*   m_adaptiveThrottleCheck{nullptr};
};

} // namespace AetherSDR
