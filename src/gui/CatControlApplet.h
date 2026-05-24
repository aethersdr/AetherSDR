#pragma once

#include <QWidget>

class QPushButton;
class QLabel;
class QLineEdit;

namespace AetherSDR {

class RadioModel;
class RigctlServer;
class RigctlPty;

// CAT Control Applet — 4-channel rigctld TCP + PTY control panel.
// Each channel (A-D) is bound to a slice index (0-3) and provides an
// independent rigctld TCP port + PTY symlink.
class CatControlApplet : public QWidget {
    Q_OBJECT

public:
    static constexpr int kChannels = 4;

    explicit CatControlApplet(QWidget* parent = nullptr);

    void setRadioModel(RadioModel* model);
    void setRigctlServers(RigctlServer** servers, int count);
    void setRigctlPtys(RigctlPty** ptys, int count);

    // Sync Enable button state (called by MainWindow on autostart)
    void setTcpEnabled(bool on);
    void setPtyEnabled(bool on);

private:
    void buildUI();
    void updateChannelStatus(int ch);
    void updateAllChannelStatus();

    RadioModel*   m_model{nullptr};
    RigctlServer* m_servers[kChannels]{};
    RigctlPty*    m_ptys[kChannels]{};

    // Global controls
    QPushButton* m_tcpEnable{nullptr};
    QPushButton* m_ptyEnable{nullptr};
    QLineEdit*   m_basePort{nullptr};

    // Per-channel status rows
    struct ChannelRow {
        QLabel* badge{nullptr};      // coloured "A"/"B"/"C"/"D"
        QLabel* tcpStatus{nullptr};  // ":4532 (1 client)" or "(stopped)"
        QLabel* ptyPath{nullptr};    // RigctlPty::defaultSymlinkPath(i)
    };
    ChannelRow m_rows[kChannels];
};

} // namespace AetherSDR
