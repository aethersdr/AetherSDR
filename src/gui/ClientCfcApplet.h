#pragma once

#include <QWidget>

class QLabel;

namespace AetherSDR {

class AudioEngine;
class ClientCfcCurveWidget;
class ClientCompKnob;

// Docked tile for the CFC stage: its curves (read-only), Pre-comp and Post
// gain, and the FFT size with the delay it adds. Bypass lives on the CHAIN
// widget; the AetherTX window hosts the curve editor.
class ClientCfcApplet : public QWidget {
    Q_OBJECT

public:
    explicit ClientCfcApplet(QWidget* parent = nullptr);
    void setAudioEngine(AudioEngine* engine);
    void refreshEnableFromEngine();

private:
    void syncFromEngine();

    AudioEngine* m_audio{nullptr};
    ClientCfcCurveWidget* m_curve{nullptr};
    ClientCompKnob* m_precomp{nullptr};
    ClientCompKnob* m_postGain{nullptr};
    QLabel* m_fftLabel{nullptr};
};

}  // namespace AetherSDR
