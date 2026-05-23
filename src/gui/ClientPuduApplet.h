#pragma once

#include <QWidget>

class QPushButton;

namespace MasterSDR {

class AudioEngine;
class ClientCompKnob;       // reused — generic rotary knob
class PooDooLogo;

// Docked tile for the PUDU exciter — centrepiece of the PooDoo Audio™
// chain.  Layout:
//
//   ┌──────── PooDoo™ logo (pulses with wet RMS) ────────┐
//   │                                                    │
//   │  ┌─────── A | B mode toggle ───────┐               │
//   │                                                    │
//   │  Poo: [Drive] [Tune] [Mix]                         │
//   │  Doo: [Tune] [Harmonics] [Mix]                     │
//   │                                                    │
//   │  [Enable]                                 [Edit…]  │
//   └────────────────────────────────────────────────────┘
class ClientPuduApplet : public QWidget {
    Q_OBJECT

public:
    enum class Side { Tx, Rx };

    explicit ClientPuduApplet(Side side = Side::Tx, QWidget* parent = nullptr);

    void setAudioEngine(AudioEngine* engine);
    void refreshEnableFromEngine();
    Side side() const { return m_side; }

signals:
    void editRequested();

private:
    void buildUI();
    void syncControlsFromEngine();
    void onEnableToggled(bool on);
    void onModeToggled(int id);

    void applyPooDrive(float db);
    void applyPooTune(float hz);
    void applyPooMix(float v);
    void applyDooTune(float hz);
    void applyDooHarmonics(float db);
    void applyDooMix(float v);

    AudioEngine*    m_audio{nullptr};
    const Side      m_side{Side::Tx};
    class ClientPudu* pudu() const;
    void              savePuduSettings() const;
    PooDooLogo*     m_logo{nullptr};
    QPushButton*    m_modeA{nullptr};
    QPushButton*    m_modeB{nullptr};
    QPushButton*    m_enable{nullptr};
    QPushButton*    m_edit{nullptr};
    ClientCompKnob* m_pooDrive{nullptr};
    ClientCompKnob* m_pooTune{nullptr};
    ClientCompKnob* m_pooMix{nullptr};
    ClientCompKnob* m_dooTune{nullptr};
    ClientCompKnob* m_dooHarmonics{nullptr};
    ClientCompKnob* m_dooMix{nullptr};
    bool            m_restoring{false};
};

} // namespace MasterSDR
