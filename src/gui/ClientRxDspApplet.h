#pragma once

#include <QWidget>

namespace MasterSDR {

class AudioEngine;
class MasterDspWidget;

// RX-side DSP applet — embeds MasterDspWidget as a docked tile inside the
// Masterial Audio (PooDoo) container.  Same control set and persistence as
// the modeless MasterDspDialog (Settings menu); both views write to the
// same AppSettings keys, so changes in one update the other on next
// syncFromEngine().
class ClientRxDspApplet : public QWidget {
    Q_OBJECT

public:
    explicit ClientRxDspApplet(QWidget* parent = nullptr);

    void setAudioEngine(AudioEngine* engine);
    MasterDspWidget* widget() const { return m_widget; }

private:
    AudioEngine*     m_audio{nullptr};
    MasterDspWidget* m_widget{nullptr};
};

} // namespace MasterSDR
