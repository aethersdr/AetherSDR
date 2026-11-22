#pragma once

#include <QWidget>
#include <QList>

class QPushButton;
class QLabel;
class QComboBox;
class QCheckBox;
class QLineEdit;

namespace AetherSDR {

class AntennaGeniusModel;
struct AgDeviceInfo;

// Antenna Genius applet — controls a 4O3A Antenna Genius switch.
//
// Layout (top to bottom):
//  - Title bar: "ANTENNA GENIUS"
//  - Device selector (if multiple devices found)
//  - Connection status label
//  - Port A section: band label, RX/TX antenna, antenna grid buttons
//  - Port B section: same layout
//  - AUTO toggle per port
//  - Bottom-right direct connection indicator (OFFLINE when disconnected)
class AntennaGeniusApplet : public QWidget {
    Q_OBJECT

public:
    explicit AntennaGeniusApplet(QWidget* parent = nullptr);

    void setModel(AntennaGeniusModel* model);

private:
    void buildUI();
    void tryManualConnect();
    void attemptDirectConnection(const AgDeviceInfo& info, bool force = false);
    void setConnectionSource(bool connected);
    void syncFromModel();
    void rebuildAntennaButtons();
    void updatePortDisplay(int portId);
    void applyAntennaSelectionMode();

    AntennaGeniusModel* m_model{nullptr};
    bool m_updatingFromModel{false};

    // Device selector (shown when multiple devices discovered)
    QComboBox*   m_deviceCombo{nullptr};
    QPushButton* m_connectBtn{nullptr};
    QLabel*      m_statusLabel{nullptr};
    QLabel*      m_sourceLabel{nullptr};

    // Manual IP entry (for remote connections without UDP discovery)
    QLineEdit*   m_manualIpEdit{nullptr};
    QCheckBox*   m_splitRxTxCheck{nullptr};

    // Port A widgets
    QLabel*      m_portABandLabel{nullptr};
    QLabel*      m_portAAntLabel{nullptr};
    QLabel*      m_portARxHeading{nullptr};
    QLabel*      m_portATxHeading{nullptr};
    QWidget*     m_portARxBtnGrid{nullptr};
    QWidget*     m_portATxBtnGrid{nullptr};
    QList<QPushButton*> m_portARxBtns;
    QList<QPushButton*> m_portATxBtns;
    QPushButton* m_portAAutoBtn{nullptr};

    // Port B widgets
    QLabel*      m_portBBandLabel{nullptr};
    QLabel*      m_portBAntLabel{nullptr};
    QLabel*      m_portBRxHeading{nullptr};
    QLabel*      m_portBTxHeading{nullptr};
    QWidget*     m_portBRxBtnGrid{nullptr};
    QWidget*     m_portBTxBtnGrid{nullptr};
    QList<QPushButton*> m_portBRxBtns;
    QList<QPushButton*> m_portBTxBtns;
    QPushButton* m_portBAutoBtn{nullptr};

    // Port sections (for hiding Port B if device has only 1 port)
    QWidget*     m_portASection{nullptr};
    QWidget*     m_portBSection{nullptr};
};

} // namespace AetherSDR
