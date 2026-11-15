#pragma once

#include <QWidget>

class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientCompKnob;

// Docked tile for the client-side TX reverb (Freeverb).  Compact
// 5-knob row — Size, Decay, Damping, PreDly, Mix — matches the PUDU
// applet footprint.  Bypass lives on the CHAIN widget single-click;
// the Aetherial Audio Channel Strip hosts the full editor.
class ClientReverbApplet : public QWidget {
    Q_OBJECT

public:
    explicit ClientReverbApplet(QWidget* parent = nullptr);

    void setAudioEngine(AudioEngine* engine);
    void refreshEnableFromEngine();

private:
    void buildUI();
    void syncKnobsFromEngine();

    AudioEngine*    m_audio{nullptr};
    ClientCompKnob* m_size{nullptr};
    ClientCompKnob* m_decay{nullptr};
    ClientCompKnob* m_damping{nullptr};
    ClientCompKnob* m_preDly{nullptr};
    ClientCompKnob* m_mix{nullptr};
    QWidget*        m_viz{nullptr};   // ReverbVizBox (defined in .cpp)
};

} // namespace AetherSDR
