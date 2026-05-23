#pragma once

#include <QWidget>

class QPushButton;
class QTimer;

namespace MasterSDR {

class AudioEngine;
class ClientCompCurveWidget;
class ClientCompGrBar;
class ClientCompKnob;

// Docked dashboard tile for the client-side TX compressor.  View-only —
// shows the transfer curve with a live "ball" at the current envelope
// level, plus a compact horizontal gain-reduction strip, plus Bypass /
// Edit… buttons.  The full interactive editor lives in a separate
// floating window (ClientCompEditor).
//
// Path is locked at construction; AppletPanel instantiates one Tx-
// bound copy and one Rx-bound copy for the two PooDoo Audio sub-
// containers.  Engine accesses route through comp() / saveCompSettings()
// so a single class serves both sides.
class ClientCompApplet : public QWidget {
    Q_OBJECT

public:
    enum class Side { Tx, Rx };

    explicit ClientCompApplet(Side side = Side::Tx, QWidget* parent = nullptr);

    void setAudioEngine(AudioEngine* engine);
    Side side() const { return m_side; }

    // Pull the Bypass toggle state from the bound ClientComp.  Called
    // by MainWindow after the floating editor toggles its bypass
    // button so both views stay in sync.
    void refreshEnableFromEngine();

signals:
    // Fired when the user clicks Edit…. MainWindow owns the single
    // editor window instance and shows / raises it in response.
    void editRequested();

private:
    void buildUI();
    void syncEnableFromEngine();
    void onEnableToggled(bool on);
    void tickMeter();

    AudioEngine*          m_audio{nullptr};
    const Side            m_side{Side::Tx};
    class ClientComp*     comp() const;
    void                  saveCompSettings() const;
    QPushButton*          m_enable{nullptr};
    QPushButton*          m_edit{nullptr};
    ClientCompCurveWidget* m_curve{nullptr};
    ClientCompGrBar*      m_grBar{nullptr};   // narrow horizontal GR strip
    // Five-knob tuning row — Thresh, Ratio, Attack, Release, Makeup.
    ClientCompKnob*       m_thresh{nullptr};
    ClientCompKnob*       m_ratio{nullptr};
    ClientCompKnob*       m_attack{nullptr};
    ClientCompKnob*       m_release{nullptr};
    ClientCompKnob*       m_makeup{nullptr};
    QTimer*               m_meterTimer{nullptr};

    // Latest GR dB (0 = no reduction, negative = active).  Paint is
    // driven by a QTimer polling ClientComp::gainReductionDb() at 30 Hz
    // so the strip feels live.
    float m_grDb{0.0f};
};

} // namespace MasterSDR
