#pragma once

#include <QWidget>
#include <QList>
#include <QTimer>

class QPushButton;
class QLabel;
class QFrame;

namespace MasterSDR {

class AntennaGeniusModel;

// ShackSwitch applet — compact antenna switcher panel for ShackSwitch devices.
//
// Detected by AG-protocol device name "ShackSwitch". Replaces the generic AG applet
// layout with a single antenna list and compact Input A / Input B header cards.
//
// Layout (top to bottom):
//  - Status row: device IP + version
//  - INPUT A card: band + current antenna name (cyan)
//  - INPUT B card: band + current antenna name (orange) — hidden on R4
//  - Antenna rows: name + [A] [B] select buttons
//  - Dummy load selector row
//  - Settings button (opens web UI)
//
// Conflict detection: when both portA and portB are on the same antenna the B
// button on that row blinks amber.  If a dummy load antenna is configured,
// B is automatically routed there and the dummy load row blinks orange while
// the intended row's B button blinks amber to show where B "wants" to be.
class ShackSwitchApplet : public QWidget {
    Q_OBJECT

public:
    explicit ShackSwitchApplet(QWidget* parent = nullptr);

    void setModel(AntennaGeniusModel* model);

private:
    void buildUI();
    void rebuildAntennaRows();
    void updateInputHeaders();
    void checkConflict(int rxA, int rxB);
    void applyButtonStyles(int rxA, int rxB, bool singlePort);
    void onBlinkTick();
    void updateDummyLoadBtn();

    AntennaGeniusModel* m_model{nullptr};
    bool m_updatingFromModel{false};

    // Status
    QLabel*  m_statusLabel{nullptr};

    // Input header cards
    QLabel*  m_inputABandLabel{nullptr};
    QLabel*  m_inputAAntLabel{nullptr};
    QLabel*  m_inputBBandLabel{nullptr};
    QLabel*  m_inputBAntLabel{nullptr};
    QWidget* m_inputBCard{nullptr};

    // Column header widgets (hidden on single-port devices)
    QLabel* m_bColumnHeader{nullptr};

    // Antenna rows container
    QWidget* m_antennaContainer{nullptr};

    // Per-antenna A/B buttons (rebuilt when antenna list changes)
    struct AntRow {
        int          antennaId{0};
        QPushButton* aBtn{nullptr};
        QPushButton* bBtn{nullptr};
    };
    QList<AntRow> m_antRows;

    // Dummy load selector
    QPushButton* m_dummyLoadBtn{nullptr};   // shows current DL antenna name
    int          m_dummyLoadAntId{-1};      // -1 = no dummy load configured

    // Conflict / blink state
    QTimer m_blinkTimer;
    bool   m_blinkState{false};
    int    m_conflictAntId{-1};    // antenna ID where A==B conflict; -1 = none
    int    m_intendedBAntId{-1};   // where B was trying to go before DL reroute
    bool   m_autoRoutedToDummy{false};
};

} // namespace MasterSDR
