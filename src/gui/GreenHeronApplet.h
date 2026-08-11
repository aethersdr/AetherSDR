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

private:
    void buildUI();
    void toggleConnection();
    void refreshSwitchChoices();
    void rebuildPortList();
    void syncFromModel();
    void updateStatus();
    void note(const QString& text);

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

    // port name → its button. Rebuilt whenever the shown switch's roster
    // changes.
    QHash<QString, QPushButton*> m_portButtons;

    // What the current port list was built from, so a redraw that changes
    // nothing does not tear the buttons down under the operator's cursor.
    QString     m_builtForSwitch;
    QStringList m_builtForPorts;

    // The operator's choice, remembered across roster arrivals and restarts.
    // Held separately from the combo because the combo is empty until the
    // device announces its switches.
    QString m_wantedSwitch;
};

} // namespace AetherSDR
