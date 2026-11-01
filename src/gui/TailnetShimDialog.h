#pragma once

#include "CanonWindow.h"

#include <QHostAddress>
#include "core/TailnetShimClient.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace AetherSDR {

class RadioModel;

// Configures the in-radio tailnet shim (the flex-tailnet-shim Docker
// waveform): shows whether the radio is on the operator's tailnet, signs it
// in with a QR code to scan on a phone (or, under Advanced, a single-use
// Tailscale auth key sent straight to the container), edits who may
// connect, and signs the radio out. Opened from the shim's row
// in the Waveforms dialog. Works only over the radio's LAN, because that is
// the only place the container's provisioning API listens.
class TailnetShimDialog : public CanonWindow {
    Q_OBJECT
public:
    explicit TailnetShimDialog(RadioModel* model, QWidget* parent = nullptr);

private:
    void refresh();
    void showStatus(const TailnetShimStatus& status);
    void showError(const QString& message);
    void showMessage(const QString& message, const QString& tone);
    void setStatusLine(const QString& text, const QString& state);
    // Sets wrapped text and reserves its height: a fixed-width CanonWindow
    // under-reports wrapped labels' height (the About window does the same).
    void setWrapped(QLabel* label, const QString& text, int width);
    void setBusy(bool busy, const QString& what = {});
    void updateControls();
    void join();
    void signInWithQr();
    void provision(const QString& authKey);
    void showSignIn(const QString& url);
    void hideSignIn();
    void onLogin(const TailnetShimLogin& login);
    void setAdvancedOpen(bool open);
    void saveAllowList();
    void saveSharing();
    void signOut();
    void onProvisioned(const TailnetShimProvisionReply& reply);
    void onRequestFailed(const QString& operation, const QString& message, bool unauthorized);

    RadioModel* m_model{nullptr};
    TailnetShimClient m_client;
    QTimer* m_pollTimer{nullptr};

    QString m_radioSerial;
    QHostAddress m_apiAddress;   // the radio's LAN address, where the API listens
    QString m_adminToken;
    QString m_tokenSaveError;   // why the keychain refused the token, if it did
    bool m_tokenLoaded{false};
    bool m_lanReachable{false};
    bool m_busy{false};
    // A QR sign-in this window started: on reaching the tailnet it says so.
    bool m_signingIn{false};
    bool m_autoSignInTried{false};   // the code is offered unasked once per window
    bool m_advancedOpenedForVersion{false};
    QString m_authUrl;   // the sign-in URL the QR code shows, if any
    std::optional<TailnetShimStatus> m_status;

    QLabel* m_statusDot{nullptr};
    QLabel* m_stateLabel{nullptr};
    QLabel* m_nameLabel{nullptr};
    QLabel* m_addressLabel{nullptr};
    QPushButton* m_copyButton{nullptr};
    QLabel* m_sessionsLabel{nullptr};
    QLabel* m_versionLabel{nullptr};
    QLabel* m_expiryLabel{nullptr};
    QLabel* m_expiryHint{nullptr};
    QLabel* m_messageLabel{nullptr};
    QLabel* m_tokenNote{nullptr};
    QLineEdit* m_keyEdit{nullptr};
    QLineEdit* m_hostnameEdit{nullptr};
    QLineEdit* m_allowEdit{nullptr};
    QPushButton* m_refreshButton{nullptr};
    QPushButton* m_signOutButton{nullptr};
    QPushButton* m_saveAllowButton{nullptr};
    QPushButton* m_joinButton{nullptr};

    // Sign-in by QR code, and the auth key under Advanced.
    QPushButton* m_qrButton{nullptr};
    QLabel* m_qrHint{nullptr};
    QWidget* m_qrPanel{nullptr};
    QLabel* m_qrImage{nullptr};
    QLabel* m_qrSteps{nullptr};
    QLabel* m_qrLink{nullptr};
    QPushButton* m_copyLinkButton{nullptr};
    QPushButton* m_advancedToggle{nullptr};
    QWidget* m_keyPanel{nullptr};

    // Station devices (4O3A accessories) shared over the tailnet.
    QLabel* m_devicesLabel{nullptr};
    QCheckBox* m_shareDiscoveredCheck{nullptr};
    QLineEdit* m_extraDevicesEdit{nullptr};
    QPushButton* m_saveSharingButton{nullptr};
    QLabel* m_sharingNote{nullptr};

    int m_bodyTextWidth{0};   // full-width text in the window body
    int m_cardTextWidth{0};   // full-width text inside a card
};

}  // namespace AetherSDR
