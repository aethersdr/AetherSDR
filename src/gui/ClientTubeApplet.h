#pragma once

#include <QWidget>

class QPushButton;
class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientCompKnob;
class ClientTubeCurveWidget;

// Docked tile for the client-side dynamic tube saturator.  Shows
// the transfer curve (which bends with Drive / Bias / Model) and a
// live input ball.  Path is locked at construction; AppletPanel
// instantiates one Tx-bound copy and one Rx-bound copy for the two
// PooDoo Audio sub-containers.  Engine accesses route through tube()
// / saveTubeSettings() so a single class serves both sides.
class ClientTubeApplet : public QWidget {
    Q_OBJECT

public:
    enum class Side { Tx, Rx };

    explicit ClientTubeApplet(Side side = Side::Tx, QWidget* parent = nullptr);

    void setAudioEngine(AudioEngine* engine);
    void refreshEnableFromEngine();
    Side side() const { return m_side; }

signals:
    void editRequested();

private:
    void buildUI();
    void syncEnableFromEngine();
    void onEnableToggled(bool on);

    AudioEngine*           m_audio{nullptr};
    const Side             m_side{Side::Tx};
    class ClientTube*      tube() const;
    void                   saveTubeSettings() const;
    QPushButton*           m_enable{nullptr};
    QPushButton*           m_edit{nullptr};
    ClientTubeCurveWidget* m_curve{nullptr};
    // Five-knob tuning row — Drive, Tone, Bias, Output, Mix (Dry/Wet).
    ClientCompKnob*        m_drive{nullptr};
    ClientCompKnob*        m_tone{nullptr};
    ClientCompKnob*        m_bias{nullptr};
    ClientCompKnob*        m_output{nullptr};
    ClientCompKnob*        m_mix{nullptr};
    // Light polling timer — keeps the knobs in sync with any changes
    // made in the floating editor (and vice versa).
    QTimer*                m_syncTimer{nullptr};
};

} // namespace AetherSDR
