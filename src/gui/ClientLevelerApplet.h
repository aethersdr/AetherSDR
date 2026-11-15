#pragma once

#include <QWidget>

class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientCompKnob;
class ClientDbHistoryWidget;

// Docked tile for the LVLR stage: its gain history, Max gain and Decay.
// Bypass lives on the CHAIN widget; the AetherTX window has every control.
class ClientLevelerApplet : public QWidget {
    Q_OBJECT

public:
    explicit ClientLevelerApplet(QWidget* parent = nullptr);
    void setAudioEngine(AudioEngine* engine);
    void refreshEnableFromEngine();

protected:
    void showEvent(QShowEvent* ev) override;
    void hideEvent(QHideEvent* ev) override;

private:
    void syncFromEngine();

    AudioEngine* m_audio{nullptr};
    ClientDbHistoryWidget* m_history{nullptr};
    ClientCompKnob* m_maxGain{nullptr};
    ClientCompKnob* m_decay{nullptr};
    QTimer* m_poll{nullptr};
};

}  // namespace AetherSDR
