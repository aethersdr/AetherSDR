#pragma once

#include "CanonWindow.h"

#include "core/WsprAgent.h"

#include <QPointer>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace AetherSDR {

class RadioModel;
class WsprAgentClient;

// Configures the WSPR receiver running on the radio (tools/wspr-agent):
// station, receive (band, antenna, decode depth, DAX channel), WSJT-X's band
// hopping (a bands-by-period grid and the gray-line width) and upload to
// WSPRnet. The receiver owns these settings; this window reads them from
// it, and Apply sends them back. The first Apply claims the receiver, and
// the admin token it issues goes to the system keychain. Opened from the
// receiver's row in the Waveforms window.
class WsprConfigureWindow : public CanonWindow {
    Q_OBJECT

public:
    explicit WsprConfigureWindow(RadioModel* model, QWidget* parent = nullptr);

private:
    void load();
    void showConfig(const WsprConfig& config, bool claimed);
    void showStatus(const WsprStatus& status);
    void closeEvent(QCloseEvent* event) override;
    WsprConfig collect() const;
    void apply();
    void setMessage(const QString& text, const QString& tone = {});
    void setStatusLine(const QString& text, const QString& state);
    void updateControls();

    QPointer<RadioModel> m_model;
    WsprAgentClient* m_client{nullptr};
    QTimer* m_statusTimer{nullptr};
    QString m_radioSerial;
    QString m_token;
    bool m_claimed{false};
    bool m_busy{false};
    std::optional<WsprConfig> m_loaded;   // as the receiver last reported it

    QLabel* m_statusDot{nullptr};
    QLabel* m_stateLabel{nullptr};
    QLineEdit* m_callsignEdit{nullptr};
    QLineEdit* m_gridEdit{nullptr};
    QComboBox* m_bandCombo{nullptr};
    QComboBox* m_antennaCombo{nullptr};
    QComboBox* m_depthCombo{nullptr};
    QComboBox* m_daxCombo{nullptr};
    QCheckBox* m_hopCheck{nullptr};
    QSpinBox* m_graySpin{nullptr};
    QTableWidget* m_hopTable{nullptr};
    QCheckBox* m_uploadCheck{nullptr};
    QLabel* m_uploadStatus{nullptr};
    QLabel* m_messageLabel{nullptr};
    QPushButton* m_applyButton{nullptr};
};

}  // namespace AetherSDR
