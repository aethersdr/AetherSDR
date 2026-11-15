#pragma once

#include <QWidget>

class QPushButton;
class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientCompKnob;
class ClientDeEssCurveWidget;
class ClientDeEssGrBar;

// Docked tile for the client-side TX de-esser.  View-only — shows the
// sidechain bandpass response curve with a live ball at the current
// centre frequency and a compact GR strip.  Bypass via the CHAIN
// widget single-click; editing via the Aetherial Audio Channel Strip.
class ClientDeEssApplet : public QWidget {
    Q_OBJECT

public:
    explicit ClientDeEssApplet(QWidget* parent = nullptr);

    void setAudioEngine(AudioEngine* engine);
    void refreshEnableFromEngine();

private:
    void buildUI();
    void syncEnableFromEngine();
    void onEnableToggled(bool on);
    void tickMeter();

    AudioEngine*            m_audio{nullptr};
    QPushButton*            m_enable{nullptr};
    ClientDeEssCurveWidget* m_curve{nullptr};
    ClientDeEssGrBar*       m_grBar{nullptr};
    // Four-knob tuning row: sibilant band (Freq+Q), trigger threshold,
    // and max reduction amount.  Covers the everyday workflow.
    ClientCompKnob*         m_freq{nullptr};
    ClientCompKnob*         m_q{nullptr};
    ClientCompKnob*         m_thresh{nullptr};
    ClientCompKnob*         m_amount{nullptr};
    QTimer*                 m_meterTimer{nullptr};

    float m_grDb{0.0f};
};

} // namespace AetherSDR
