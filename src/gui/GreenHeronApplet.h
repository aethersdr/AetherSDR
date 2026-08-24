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
// The reference GTK client draws a full Cairo compass rose for this. In the
// DOCKED tile it is still refused: a recognisable rose would cost most of a
// 248px-wide rail's height to restate one number the readout already gives.
//
// It is drawn only when the tile is FLOATING, where the operator has sized
// the window themselves and the height is theirs to spend — see RotorCompass
// below, and the dockModeChanged wiring on the GHE entry in AppletPanel. The
// floating window resizes itself around the dial as the rotator comes and
// goes, so no default float size is declared for it. The other half of the
// original objection, that a rose's
// colours arrive as hardcoded hex in a repo that ratchets against exactly
// that, is answered by painting from ThemeManager::color() tokens rather
// than by not painting.
//
// The rose is READ-ONLY, and that is invariant 1 above rather than an
// oversight. A compass rose's natural affordance is click-to-point, which is
// precisely the single gesture that may not transmit; RotorCompass therefore
// has no mouse handlers at all, and must not acquire one — not even to fill
// the heading field, which is one Enter away from a rotation that cannot be
// recalled.

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

// The rotator dial. Shown only while the GHE tile is floating.
//
// It renders exactly what the readout renders and claims nothing more: a
// needle at the REPORTED heading, and — when this session has commanded one —
// a dimmer ghost tick at the heading that was asked for. The two marks simply
// sit where they sit. There is deliberately no arrival state, no "on target"
// colour and no tolerance ring: the hardware stops ~2° short along its
// direction of travel and wanders ±3.8° while mechanically stationary, so any
// such threshold would flicker and would be testing an error smaller than the
// measurement.
//
// Neither mark is drawn in an accent colour for the same reason. Success and
// warning tokens would editorialise about a difference the protocol offers no
// way to judge, so the needle is simply the primary text colour and the ghost
// the label colour.
class RotorCompass : public QWidget {
    Q_OBJECT

public:
    explicit RotorCompass(QWidget* parent = nullptr);

    // `reported` is what POINT last said. `asked` is drawn only when
    // `hasAsked`, and is never used to move the needle.
    void setHeading(double reported, bool hasAsked, double asked);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // For tests: what the dial is currently drawing. Due north is 0.0, which
    // is why the change guard in setHeading() cannot use qFuzzyCompare.
    double reportedHeading() const { return m_reported; }
    bool   hasAskedHeading() const { return m_hasAsked; }
    double askedHeading() const { return m_asked; }

protected:
    void paintEvent(QPaintEvent* ev) override;

    // NOTE: no mousePressEvent / mouseMoveEvent, by design. See the header
    // comment above — a click on this widget must not be able to aim a
    // rotator, directly or by filling the field that Turn reads.

private:
    double m_reported{0.0};
    double m_asked{0.0};
    bool   m_hasAsked{false};
};

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

    // Docked vs floating. Drives the compass and nothing else: every control
    // in the tile is present and behaves identically either way, so a tile
    // that is never floated loses nothing. Wired to
    // ContainerWidget::dockModeChanged where the GHE entry is built.
    void setFloating(bool on);

    // Exposed for tests: the compass exists in both modes and is hidden in
    // the docked one, so its visibility is the thing worth asserting.
    const RotorCompass* compass() const { return m_compass; }

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

    // Lives INSIDE m_rotorSection rather than beside it, so it inherits that
    // section's gate for free: the section is shown only while a rotator is
    // reporting and is torn down after kRotorSilentAfterMs of silence. A
    // compass parented anywhere else would need its own copy of that rule,
    // and getting it wrong leaves a needle frozen at a stale heading after
    // the controller is switched off — the exact failure the gate exists for.
    RotorCompass* m_compass{nullptr};

    // Explicitly hidden at build time, so that showing m_rotorSection in the
    // docked tile does not bring the compass up with it.
    bool m_floating{false};

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
