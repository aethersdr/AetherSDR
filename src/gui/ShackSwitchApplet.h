#pragma once

#include <QWidget>
#include <QList>
#include <QTimer>

class QPushButton;
class QLabel;
class QFrame;

namespace AetherSDR {

class AntennaGeniusModel;

// Compact antenna switcher for AG-protocol devices named "ShackSwitch",
// replacing the generic AG applet layout. Input B card is hidden on R4.
// Conflict display: when portA and portB share an antenna the B button blinks
// amber; with a dummy load configured, B is routed there, the dummy row blinks
// orange and the intended row's B button blinks amber.
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

} // namespace AetherSDR
