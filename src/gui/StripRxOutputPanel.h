#pragma once

#include <QElapsedTimer>
#include <QWidget>

class QHideEvent;
class QShowEvent;

#include "MeterSmoother.h"

class QLabel;
class QPushButton;
class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientCompKnob;

// "Aetherial Output - RX": RX counterpart of StripFinalOutputPanel at the end of
// the RX grid (#2425). No limiter/test-tone/Quindar (TX-only). Shows a peak/RMS
// meter from the RX scope tap (post-Pudu, what reaches the local sink), MUTE
// (master local mute) and BOOST (#1445 soft-knee tanh, RX-only).
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

protected:
    // The animation tick stops while the page is hidden — see PanelTick.h.
    void showEvent(QShowEvent* ev) override;
    void hideEvent(QHideEvent* ev) override;

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

    // Throttle the PK / RMS / CRST QLabel updates to the project-
    // canonical 10 Hz cadence (kMeterReadoutUpdateMs) so the digits
    // are readable while the bar continues to animate every tick.
    qint64          m_lastReadoutUpdateMs{0};
};

} // namespace AetherSDR
