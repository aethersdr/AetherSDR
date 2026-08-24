#pragma once

// GreenHeronApplet — the GHE tile: the ONE Green Heron "Everyware" switch this
// station's radio is fed from, as a list of its antenna ports.
//
// The Everyware server presents several switches (four on the reference
// installation) and the reference Python client draws all of them as a
// matrix. This applet deliberately does not: a radio is wired to exactly one
// switch, so the operator picks theirs once and then sees a single column of
// real antenna names — which is also what fits a 260 px sidebar tile without
// abbreviating everything into initials.
//
// The other switches are not ignored, they are just not drawn. Their
// selections arrive in SWITCHLOCKS and are what marks an antenna "in use by
// AS-84F-2" here, because an antenna feeds one switch at a time and the
// device would refuse the click anyway.
//
// The applet OWNS its GreenHeronModel rather than being handed one by
// MainWindow (the AntennaGenius pattern). Two reasons: the Everyware server
// has no discovery path, so there is nothing for MainWindow to detect and
// nothing to condition the tray button on — the operator types an address and
// the applet is the only thing that needs it; and the switch is
// radio-agnostic, so no MainWindow signal has to reach it. That keeps the
// whole feature to three files plus its registration.
//
// The list renders exactly what the device has reported — never what we asked
// for. A relay that fails to move shows up as a button that does not light,
// rather than as a UI that lies (see GreenHeronModel::selectPort).
//
// THE ROTATOR SITS IN THIS SAME TILE, under the antenna list, because the
// Everyware server carries it down the same socket the switches use. A
// separate applet would have to open a second connection to the same server
// to show one number.
//
// It appears only while the device is actually reporting a heading and
// vanishes when it stops — that is not a cosmetic choice. A rotator is
// announced only while its controller is powered on, and the controller going
// off is not a socket event: the heading would otherwise sit there
// indefinitely beside a Turn button that would aim a controller that is off.
//
// Two things about it are safety invariants rather than preferences, and both
// are easy to "tidy" away:
//
//   1. CHOOSING A HEADING AND SENDING IT ARE SEPARATE GESTURES. Typing a
//      heading proposes it; Turn (or Enter in the field) transmits. There is
//      no stop or park verb anywhere in this protocol, so a rotation that
//      starts cannot be recalled in software. Nothing here may transmit on a
//      single click or a drag.
//   2. THE READOUT IS THE REPORTED HEADING, NEVER THE COMMANDED ONE, and it
//      never says "on target". Measured on real hardware: the rotator stops
//      about two degrees short along whichever way it was turning, overshoots
//      on the way there, and wanders over a ±3.8° band while mechanically
//      stationary. Any arrival threshold at that noise level would flicker,
//      and the error it tested would be smaller than the measurement. Hence
//      "62.9° · asked 64.3° · Δ1.4°" and no verdict.
//
// The reference GTK client draws a full Cairo compass rose for this. A
// recognisable rose would cost most of this tile's height for one number, and
// its colours would arrive as hardcoded hex in a repo that ratchets against
// exactly that — so what is ported is the dial's information and its safety
// model, not its pixels.

#include <QHash>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class QVBoxLayout;

namespace AetherSDR {

class GreenHeronModel;

class GreenHeronApplet : public QWidget {
    Q_OBJECT

public:
    explicit GreenHeronApplet(QWidget* parent = nullptr);

    // Exposed for tests and the automation bridge; the applet creates and
    // owns it.
    GreenHeronModel* model() const { return m_model; }

    // The switch whose ports are on screen. Empty until one is chosen.
    QString selectedSwitch() const;

    // The rotator whose heading is on screen, or empty when the device is not
    // reporting one. Exposed for tests and the automation bridge.
    QString selectedRotor() const;

private:
    void buildUI();
    void toggleConnection();
    void refreshSwitchChoices();
    // The switch actually driven and drawn: the operator's choice while the
    // device still offers it, otherwise the first switch on the roster. Kept
    // apart from m_wantedSwitch so a roster that is still arriving cannot
    // overwrite what the operator asked for.
    QString effectiveSwitch() const;
    void rebuildPortList();
    void syncFromModel();
    // The rotator half: which one is shown, what the readout says, and the
    // one place a TURN is ever sent from.
    QString effectiveRotor() const;
    void refreshRotorChoices();
    void syncRotorFromModel();
    void sendTurn();
    void updateStatus();
    void note(const QString& text);
    void showNote(const QString& text, const QString& colourToken);

    GreenHeronModel* m_model{nullptr};

    QLineEdit*   m_hostEdit{nullptr};
    QSpinBox*    m_portSpin{nullptr};
    QComboBox*   m_switchCombo{nullptr};
    QPushButton* m_connectBtn{nullptr};
    QLabel*      m_statusLabel{nullptr};
    QLabel*      m_noteLabel{nullptr};
    QTimer*      m_noteTimer{nullptr};

    QWidget*     m_portHost{nullptr};
    QVBoxLayout* m_portLayout{nullptr};

    // Hidden as a whole whenever no rotator is reporting — see the header
    // comment. There is no "rotator offline" placeholder because the device
    // does not distinguish "absent" from "powered off"; silence is all it has.
    QWidget*     m_rotorSection{nullptr};
    QComboBox*   m_rotorCombo{nullptr};
    QLabel*      m_rotorReadout{nullptr};
    QLineEdit*   m_headingEdit{nullptr};
    QPushButton* m_turnBtn{nullptr};

    // port name → its button. Rebuilt whenever the shown switch's roster
    // changes.
    QHash<QString, QPushButton*> m_portButtons;

    // What the current port list was built from, so a redraw that changes
    // nothing does not tear the buttons down under the operator's cursor.
    QString     m_builtForSwitch;
    QStringList m_builtForPorts;

    // The operator's choice, remembered across roster arrivals and restarts.
    // Held separately from the combo because the combo is empty until the
    // device announces its switches. Written only by the operator picking from
    // the combo and by the settings load — never from an arriving roster, which
    // spans several reads and is therefore incomplete for a time.
    QString m_wantedSwitch;

    // The operator's rotator choice, when the server reports more than one.
    // Not persisted: a rotator exists only while its controller is on, so
    // there is nothing stable to remember, and the common case is one.
    QString m_wantedRotor;

    // Headings this session has commanded, per rotator. Kept ONLY to render
    // "asked X · ΔY" beside the reported heading — never fed back into the
    // readout itself, and never compared against a tolerance to declare
    // arrival.
    QHash<QString, double> m_commanded;

    // What the readout last said, so a screen reader is not told the heading
    // once a second. The device pushes POINT at ~0.97 s and the value dithers
    // at rest, so announcing every change would make the tile unusable with
    // Orca / NVDA; only the rotator appearing, disappearing, or a command
    // going out is worth an announcement.
    QString m_lastRotorAnnouncement;
};

} // namespace AetherSDR
