#pragma once

#include <QWidget>

class QPushButton;
class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientCompKnob;
class ClientGateCurveWidget;
class ClientGateGrBar;

// Docked, view-only tile for the client-side gate / expander: transfer curve
// with a live input-level ball plus a gain-reduction strip. Editing lives in
// ClientGateEditor. Path is fixed at construction (AppletPanel builds one Tx and
// one Rx copy); all engine access goes through gate() / save().
class ClientGateApplet : public QWidget {
    Q_OBJECT

public:
    enum class Side { Tx, Rx };

    explicit ClientGateApplet(Side side = Side::Tx, QWidget* parent = nullptr);

    void setAudioEngine(AudioEngine* engine);
    Side side() const { return m_side; }

    // Pull the Enable toggle state from the bound ClientGate.  Called
    // by MainWindow after the floating editor toggles its bypass button
    // so both views stay in sync.
    void refreshEnableFromEngine();

signals:
    void editRequested();

private:
    void buildUI();
    void syncEnableFromEngine();
    void onEnableToggled(bool on);
    void tickMeter();

    AudioEngine*           m_audio{nullptr};
    const Side             m_side{Side::Tx};
    class ClientGate*      gate() const;
    void                   saveGateSettings() const;
    QPushButton*           m_enable{nullptr};
    QPushButton*           m_edit{nullptr};
    ClientGateCurveWidget* m_curve{nullptr};
    ClientGateGrBar*       m_grBar{nullptr};
    // Five most-tuned knobs mirroring the editor: threshold, ratio,
    // return, release, floor.  Saves a trip to the floating editor for
    // everyday tuning.
    ClientCompKnob*        m_thresh{nullptr};
    ClientCompKnob*        m_ratio{nullptr};
    ClientCompKnob*        m_return{nullptr};
    ClientCompKnob*        m_release{nullptr};
    ClientCompKnob*        m_floor{nullptr};
    QTimer*                m_meterTimer{nullptr};

    float m_grDb{0.0f};
};

} // namespace AetherSDR
