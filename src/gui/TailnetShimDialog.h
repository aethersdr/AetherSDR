#pragma once

#include "PersistentDialog.h"
#include "core/TailnetShimClient.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace AetherSDR {

class RadioModel;

// Configures the in-radio tailnet shim (the flex-tailnet-shim Docker
// waveform): shows whether the radio is on the operator's tailnet, takes a
// single-use Tailscale auth key and sends it straight to the container,
// edits who may connect, and signs the radio out. Opened from the shim's row
// in the Waveforms dialog. Works only over the radio's LAN, because that is
// the only place the container's provisioning API listens.
class TailnetShimDialog : public PersistentDialog {
    Q_OBJECT
public:
    explicit TailnetShimDialog(RadioModel* model, QWidget* parent = nullptr);

private:
    void refresh();
    void showStatus(const TailnetShimStatus& status);
    void showError(const QString& message);
    void setBusy(bool busy, const QString& what = {});
    void updateControls();
    void join();
    void saveAllowList();
    void signOut();

    RadioModel* m_model{nullptr};
    TailnetShimClient m_client;
    QTimer* m_pollTimer{nullptr};

    QString m_radioSerial;
    QString m_adminToken;
    bool m_tokenLoaded{false};
    bool m_lanReachable{false};
    bool m_busy{false};
    std::optional<TailnetShimStatus> m_status;

    QLabel* m_stateLabel{nullptr};
    QLabel* m_nameLabel{nullptr};
    QLabel* m_addressLabel{nullptr};
    QPushButton* m_copyButton{nullptr};
    QLabel* m_sessionsLabel{nullptr};
    QLabel* m_versionLabel{nullptr};
    QLabel* m_messageLabel{nullptr};
    QLabel* m_tokenNote{nullptr};
    QLineEdit* m_keyEdit{nullptr};
    QLineEdit* m_hostnameEdit{nullptr};
    QLineEdit* m_allowEdit{nullptr};
    QPushButton* m_refreshButton{nullptr};
    QPushButton* m_signOutButton{nullptr};
    QPushButton* m_saveAllowButton{nullptr};
    QPushButton* m_joinButton{nullptr};
};

}  // namespace AetherSDR
