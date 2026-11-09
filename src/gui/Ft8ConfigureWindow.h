#pragma once

#include "CanonWindow.h"

#include "core/Ft8Agent.h"

#include <QPointer>
#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace AetherSDR {

class RadioModel;
class Ft8AgentClient;

// Configures the FT8 receiver running on the radio (tools/ft8-agent):
// station, and receive (band, antenna, DAX channel). The receiver owns these
// settings; this window reads them from it, and Apply sends them back. The
// first Apply claims the receiver, and the admin token it issues goes to the
// system keychain. Opened from the receiver's row in the Waveforms window.
//
// ⚠ Deliberately narrower than WsprConfigureWindow, because the FT8 agent
// takes a strict subset of the settings: there is no band hopping (WSPR's
// schedule is WSJT-X's two-minute slot rotation and has no FT8 meaning), no
// gray-line width, no decode depth and no upload. A capability-driven window
// shared by every shim is proposed in RFC #7114; this proves the control path
// first so that design has a second real instance to generalise from.
//
// RECEIVE ONLY: nothing here can key a radio. The agent has no transmit path.
class Ft8ConfigureWindow : public CanonWindow {
    Q_OBJECT

public:
    explicit Ft8ConfigureWindow(RadioModel* model, QWidget* parent = nullptr);

private:
    void load();
    void showConfig(const Ft8Config& config, bool claimed);
    void showStatus(const Ft8Status& status);
    void closeEvent(QCloseEvent* event) override;
    Ft8Config collect() const;
    void apply();
    void setMessage(const QString& text, const QString& tone = {});
    void setStatusLine(const QString& text, const QString& state);
    void updateControls();

    QPointer<RadioModel> m_model;
    Ft8AgentClient* m_client{nullptr};
    QTimer* m_statusTimer{nullptr};
    QString m_radioSerial;
    QString m_token;
    bool m_claimed{false};
    bool m_busy{false};
    std::optional<Ft8Config> m_loaded;   // as the receiver last reported it

    QLabel* m_statusDot{nullptr};
    QLabel* m_stateLabel{nullptr};
    QLabel* m_decodeLabel{nullptr};      // decode time against the cycle budget
    QLineEdit* m_callsignEdit{nullptr};
    QLineEdit* m_gridEdit{nullptr};
    QComboBox* m_bandCombo{nullptr};
    QComboBox* m_antennaCombo{nullptr};
    QComboBox* m_daxCombo{nullptr};
    QLabel* m_messageLabel{nullptr};
    QPushButton* m_applyButton{nullptr};
};

}  // namespace AetherSDR
