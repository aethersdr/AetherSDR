#pragma once

#include <QWidget>
#include <QList>

class QPushButton;
class QLabel;
class QComboBox;
class QLineEdit;

namespace MasterSDR {

class AntennaGeniusModel;

// Antenna Genius applet — controls a 4O3A Antenna Genius switch.
//
// Layout (top to bottom):
//  - Title bar: "ANTENNA GENIUS"
//  - Device selector (if multiple devices found)
//  - Connection status label
//  - Port A section: band label, RX/TX antenna, antenna grid buttons
//  - Port B section: same layout
//  - AUTO toggle per port
class AntennaGeniusApplet : public QWidget {
    Q_OBJECT

public:
    explicit AntennaGeniusApplet(QWidget* parent = nullptr);

    void setModel(AntennaGeniusModel* model);

private:
    void buildUI();
    void tryManualConnect();
    void syncFromModel();
    void rebuildAntennaButtons();
    void updatePortDisplay(int portId);

    AntennaGeniusModel* m_model{nullptr};
    bool m_updatingFromModel{false};

    // Device selector (shown when multiple devices discovered)
    QComboBox*   m_deviceCombo{nullptr};
    QPushButton* m_connectBtn{nullptr};
    QLabel*      m_statusLabel{nullptr};

    // Manual IP entry (for remote connections without UDP discovery)
    QLineEdit*   m_manualIpEdit{nullptr};

    // Port A widgets
    QLabel*      m_portABandLabel{nullptr};
    QLabel*      m_portAAntLabel{nullptr};
    QWidget*     m_portABtnGrid{nullptr};
    QList<QPushButton*> m_portABtns;
    QPushButton* m_portAAutoBtn{nullptr};

    // Port B widgets
    QLabel*      m_portBBandLabel{nullptr};
    QLabel*      m_portBAntLabel{nullptr};
    QWidget*     m_portBBtnGrid{nullptr};
    QList<QPushButton*> m_portBBtns;
    QPushButton* m_portBAutoBtn{nullptr};

    // Port sections (for hiding Port B if device has only 1 port)
    QWidget*     m_portASection{nullptr};
    QWidget*     m_portBSection{nullptr};
};

} // namespace MasterSDR
