#pragma once

#include <QWidget>

namespace AetherSDR {

class AudioEngine;
class AetherDspWidget;

// RX-side DSP applet — embeds AetherDspWidget as a docked tile inside the
// Aetherial Audio (PooDoo) container.  Same control set and persistence as
// the modeless AetherRxDialog (Settings menu); both views write to the
// same AppSettings keys, so changes in one update the other on next
// syncFromEngine().
class ClientRxDspApplet : public QWidget {
    Q_OBJECT

public:
    explicit ClientRxDspApplet(QWidget* parent = nullptr);

    void setAudioEngine(AudioEngine* engine);
    AetherDspWidget* widget() const { return m_widget; }

private:
    AudioEngine*     m_audio{nullptr};
    AetherDspWidget* m_widget{nullptr};
};

} // namespace AetherSDR
