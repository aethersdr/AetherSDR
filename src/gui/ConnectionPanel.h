#pragma once

#include "core/RadioDiscovery.h"
#include "core/SmartLinkClient.h"
#include "core/IConnectionAutomation.h"

#include <QWidget>
#include <QMargins>
#include <QPoint>
#include <QRect>
#include <QSize>
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
class QScreen;
class QScrollArea;

namespace AetherSDR {

// Novice-first dialog for local, SmartLink, and manual/VPN radio connections.
class ConnectionPanel : public QWidget, public IConnectionAutomation {
    Q_OBJECT

public:
    explicit ConnectionPanel(QWidget* parent = nullptr);

    static constexpr int kSafeMinimumWidth = 640;
    static constexpr int kSafeMinimumHeight = 360;
    static constexpr int kPreferredWidth = 760;
    static constexpr int kPreferredHeight = 660;

    void setFramelessMode(bool on);
    void fitToScreen(QScreen* preferredScreen = nullptr);
    // Fit, then pull the frame back inside the work area without otherwise
    // moving the window. The placement-preserving counterpart to
    // MainWindow::showConnectionDialog(), for the show paths this class owns
    // (the frameless toggle, the automation bridge) and for the screen/DPI/font
    // changes that can invalidate a fit made earlier (#4515).
    void fitAndClampToScreen(QScreen* preferredScreen = nullptr);
    QMargins screenFitFrameMargins() const;
    QSize screenFitFrameSize() const;
    QPoint constrainedFrameTopLeft(const QPoint& preferredFrameTopLeft,
                                   const QRect& availableGeometry) const;
    void setConnected(bool connected);
    void setStatusText(const QString& text);
    void probeRadio(const QString& ip, bool restoreSavedFamily = false);

    // Radio families the "Connect by IP" page can dial. The manual page can no
    // longer guess: a FlexRadio answers TCP/4992 and a Hermes-Lite 2 answers
    // UDP/1024 (HPSDR Protocol 1), so the operator picks the wire protocol and
    // we probe exactly that one. Values match RadioInfo::family.
    static constexpr const char* kFamilyFlex = "flex";
    static constexpr const char* kFamilyHl2  = "hl2";
    // Icom networked radios (IC-705 over WiFi, IC-7300MK2 over Ethernet, …).
    // Unlike the other two this family cannot be probed anonymously: the RS-BA1
    // handshake needs a username and password before the radio will answer with
    // anything useful, which is why the manual page grows credential fields.
    static constexpr const char* kFamilyIcom = "icom";

    // IConnectionAutomation — engine-facing connect/disconnect/dialog hook.
    QList<RadioInfo> automationLocalRadios() const override;
    bool automationConnectLocalSerial(const QString& serial, QString* error = nullptr) override;
    bool automationConnectByIp(const QString& hostOrIp,
                               const QString& family = QString(),
                               QString* error = nullptr) override;
    bool automationDisconnect(QString* error = nullptr) override;
    bool automationDialogVisible() const override { return isVisible(); }
    void automationSetDialogVisible(bool visible) override
    {
        if (visible) {
            // Same fit-and-place contract MainWindow::showConnectionDialog()
            // applies, minus the re-centring: an agent-driven show must not
            // yank a window the operator positioned, only pull it back inside
            // the work area if it no longer fits there.
            fitAndClampToScreen();
            show();
            raise();
            activateWindow();
        } else {
            hide();
        }
    }
    QObject* asQObject() override { return this; }

protected:
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* e) override;

public slots:
    void onRadioDiscovered(const RadioInfo& radio);
    void onRadioUpdated(const RadioInfo& radio);
    void onRadioLost(const QString& serial);

    // Demo mode (RFC #4288): inject a synthetic "AetherSDR Demo — Simulator"
    // entry into the local radio list so a user with no radio can connect to it
    // and see the app work. Deduped by serial via onRadioDiscovered, so calling
    // it repeatedly is safe. removeDemoRadio() takes it back out (Help toggle).
    void addDemoRadio();
    void removeDemoRadio();

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
    // Keep the demo entry sorted last so real radios take precedence (RFC #4288).
    void moveDemoRadioToBottom();

    enum ConnectionMode {
        LocalMode = 0,
        SmartLinkMode = 1,
        ManualMode = 2
    };

    void setCurrentMode(ConnectionMode mode);
    void updateLocalPageState();
    void updateSmartLinkUi();
    // Right-click menu on a discovered radio row: set/clear a client-side
    // nickname (non-Flex families only). pos is in m_radioList viewport coords.
    void showRadioContextMenu(const QPoint& pos);
    void updateActionState();
    void updateLowBandwidthVisibility();
    void updateManualAdvancedVisibility();
    void refreshManualSourceOptions(const RadioBindSettings* selected = nullptr);
    // `restoreFamily` decides whether the per-address profile is allowed to move
    // the Radio type selector. FALSE on the keystroke path, and that is the
    // whole point: the recent-IP combo is editable, so Qt gives it an INLINE
    // completer, and typing one character can complete the field to a whole
    // saved address. textChanged then fired with an address the operator never
    // finished typing, this restored that address's family, and the Radio type
    // selector changed under them mid-keystroke (the operator picks Icom, types
    // "1", the field completes to a saved Flex address, and the selector jumps
    // back to FlexRadio). The restore is still right when the operator PICKS an
    // address — that is the `activated` path — and at startup.
    void applySavedSourceSelection(const QString& ip, bool restoreFamily = true);
    RadioBindSettings currentManualBindSettings(bool* staleSelection = nullptr) const;
    void loadRecentManualIps();
    void rememberManualIp(const QString& ip);
    // Radio-type selector on the manual page (persisted globally, and per-IP in
    // the routed profile so picking a recent address restores its family).
    QString currentManualFamily() const;
    void setManualFamily(const QString& family);
    void updateManualFamilyHints();
    // Directed (unicast) Metis discovery against one host.
    //
    // Three outcomes, not two: "nothing answered" and "we never got to ask" need
    // different messages, and collapsing them into a bool sent the operator to
    // power-cycle a radio that was never contacted. Only NoAnswer leaves the
    // error message to the caller — the other two have already reported.
    enum class Hl2ProbeResult {
        Answered,      // an HL2 replied; connect or refusal already reported
        NoAnswer,      // nothing replied within the deadline; caller owns the message
        NotAttempted,  // never got to ask — bind, resolve or send failed; reported here
    };
    Hl2ProbeResult probeHermesLite2(const QString& ip, const RadioBindSettings& bindSettings);
    void probeFlexRadio(const QString& ip, const RadioBindSettings& bindSettings);
    void resetManualConnectButton();
    // Re-activate the body layout after a page change. The overlap this used to
    // guard against — the Advanced section expanding, or the result line
    // wrapping, while the dialog could not grow — is now structurally
    // impossible: the body lives in a QScrollArea, which never hands its widget
    // less than qSmartMinSize(). Cheap and idempotent; safe to call often.
    void refitToContent();
    // Height the body would like if the screen allows it. The panel's own
    // sizeHint() cannot answer this — QScrollArea caps its hint by design, so
    // it under-reports the body it is scrolling.
    int preferredClientHeight() const;
    QScreen* screenFitTarget(QScreen* preferredScreen) const;
    void saveManualProfile(const QString& targetIp,
                           const RadioBindSettings& settings,
                           const QHostAddress& lastSuccessfulLocalIp);
    void saveLowBandwidthPreference(bool enabled);
    void setManualMessage(const QString& text, bool error = false);
    QString formatLocalRadioLabel(const RadioInfo& radio) const;
    QString formatWanRadioLabel(const WanRadioInfo& radio) const;

    QWidget*     m_titleBar{nullptr};
    QVBoxLayout* m_rootLayout{nullptr};
    QScrollArea* m_bodyScroll{nullptr};
    QWidget*     m_bodyContent{nullptr};
    // Last height fitToScreen() chose, so it can tell its own sizing from a
    // height the operator dragged to. It grows the dialog back toward the
    // body's preferred height only from the former — a size the operator
    // picked is theirs to keep, even if that means the body scrolls.
    int          m_autoFitHeight{kPreferredHeight};

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
    QComboBox*   m_manualRadioTypeCombo{nullptr};
    QLabel*      m_manualHintLabel{nullptr};
    QComboBox*   m_manualIpCombo{nullptr};
    // Icom credentials. The row CONTAINERS are held so the pair can be hidden
    // as a unit for every other family — hiding only the field would leave two
    // orphan labels behind.
    QWidget*     m_manualIcomUserRow{nullptr};
    QWidget*     m_manualIcomPassRow{nullptr};
    QWidget*     m_manualIcomCivRow{nullptr};
    // The hex entry's own row, shown only for "Custom...". Separate from the
    // combo's row so the two can be hidden independently — the label column is
    // measured across hidden rows too, so revealing this one does not move the
    // Radio type and Radio IP fields sideways.
    QWidget*     m_manualIcomCivCustomRow{nullptr};
    QLineEdit*   m_manualIcomUserEdit{nullptr};
    QLineEdit*   m_manualIcomPassEdit{nullptr};
    // The model chooser. Non-editable: it enumerates a known set with an escape
    // hatch, which is populateSerialPortCombo's job, not m_manualIpCombo's
    // recent-values history.
    QComboBox*   m_manualIcomCivCombo{nullptr};
    QLineEdit*   m_manualIcomCivEdit{nullptr};
    void         populateIcomCivCombo();
    void         syncIcomCivCustomRow();
    // Staged by probeRadio(), committed by setConnected(true), discarded on
    // failure. A password is only worth persisting once the radio has said it
    // is the right one.
    // Drop a staged Icom credential that did not belong to the connect that
    // actually happened. See setConnected().
    void clearPendingIcomCredentials();

    QString      m_pendingIcomPassword;
    QString      m_pendingIcomHost;
    QString      m_pendingIcomResolvedHost;
    RadioBindSettings m_pendingIcomBindSettings;
    QHostAddress m_pendingIcomSessionBindAddress;
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
    QCheckBox*   m_showDemoCheck{nullptr};    // RFC #4288: offer the demo entry

    QWidget*     m_linkOptionsWidget{nullptr};
    QLabel*      m_lowBwHintLabel{nullptr};
    QCheckBox*   m_lowBwCheck{nullptr};
    QCheckBox*   m_adaptiveThrottleCheck{nullptr};
};

} // namespace AetherSDR
