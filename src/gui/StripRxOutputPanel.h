#pragma once

#include <QElapsedTimer>
#include <QWidget>

#include "MeterSmoother.h"

class QLabel;
class QPushButton;
class QTimer;

namespace MasterSDR {

class AudioEngine;
class ClientCompKnob;

// "Masterial Output — RX" — RX-side counterpart of the TX
// `StripFinalOutputPanel`.  Sits at the very end of the RX panel grid,
// directly mirroring the TX final-output placement (#2425).
//
// The RX path has no brickwall limiter / test-tone / Quindar machinery
// (that's TX-only), so this panel is intentionally lighter than its
// TX twin — it shows what the operator can actually see and act on at
// the RX output stage:
//
//   - Live peak / RMS meter computed from the engine's RX scope tap
//     (post-Pudu, exactly what hits the local audio sink).
//   - MUTE toggle (master local audio mute).
//   - BOOST toggle (#1445 soft-knee tanh boost, RX-only).
//
// Same visual chrome (frameless title bar) as the rest of the strip
// panels.
class StripRxOutputPanel : public QWidget {
    Q_OBJECT

public:
    explicit StripRxOutputPanel(AudioEngine* engine, QWidget* parent = nullptr);
    ~StripRxOutputPanel() override;

    void showForRx();

    // Match the other strip panels' API surface — there are no
    // engine-driven knobs here, but the strip iterates every panel
    // uniformly when applying a preset.
    void syncControlsFromEngine();

private:
    void onScopeSamples(const QByteArray& monoFloat32, int sampleRate, bool tx);
    void tick();

    AudioEngine*  m_audio{nullptr};
    QWidget*        m_titleBar{nullptr};   // EditorFramelessTitleBar*
    ClientCompKnob* m_trim{nullptr};
    QPushButton*    m_muteBtn{nullptr};
    QPushButton*    m_boostBtn{nullptr};
    QWidget*        m_meter{nullptr};      // gradient bar widget (paint event)
    QLabel*         m_peakLbl{nullptr};
    QLabel*         m_rmsLbl{nullptr};
    QLabel*         m_crestLbl{nullptr};
    QTimer*         m_animTimer{nullptr};
    QElapsedTimer   m_animClock;

    // Project-canonical MeterSmoother ballistics — 30 ms attack /
    // 180 ms release at 120 Hz polling.  Targets are normalised
    // [0, 1] via dbToRatio(); m_peakDb / m_rmsDb are derived back to
    // dB for the readout labels and as references into the gradient
    // meter widget.
    MeterSmoother   m_peakSmooth;
    MeterSmoother   m_rmsSmooth;
    float           m_peakDb{-120.0f};
    float           m_rmsDb{-120.0f};
};

} // namespace MasterSDR
