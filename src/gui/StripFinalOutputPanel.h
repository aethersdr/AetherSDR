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

// "Final Output Stage" panel — the very last tile in the channel
// strip, sitting after PUDU / Reverb.  Houses:
//   - A horizontal level meter showing the audio AS THE RADIO WILL
//     RECEIVE IT (i.e. tapped at the post-final-limiter point in
//     AudioEngine, after every chain stage AND after PC mic gain).
//   - An Enable toggle + Ceiling knob for the dedicated final-stage
//     brickwall limiter (ClientFinalLimiter), which sits at the tail
//     of the chain to ensure no sample escapes louder than the
//     configured ceiling.
//   - A "LIMIT" indicator that glows amber while the limiter is
//     actively clamping.
class StripFinalOutputPanel : public QWidget {
    Q_OBJECT

public:
    explicit StripFinalOutputPanel(AudioEngine* engine,
                                   QWidget* parent = nullptr);
    ~StripFinalOutputPanel() override;

    void showForTx();
    void syncControlsFromEngine();

    // Signal-driven QUIN chip flash (#2262).  MainWindow connects this
    // slot to TransmitModel::quindarActiveChanged so the chip lights
    // bright the moment a Quindar tone starts and dims when it ends —
    // no polling.  `active` true == chip flashes; false == idle styling.
    void setQuindarActive(bool active);

private:
    void applyEnable(bool on);
    void applyCeiling(float db);
    void applyTrim(float db);
    void applyDcBlock(bool on);
    void applyTestToneEnabled(bool on);
    void applyTestToneFreq(float hz);
    void applyTestToneLevel(float db);
    void showToneEditor();
    void applyQuindarEnabled(bool on);
    void showQuindarEditor();
    void tickMeters();

    AudioEngine*    m_audio{nullptr};
    QPushButton*    m_enable{nullptr};
    QPushButton*    m_dcBtn{nullptr};
    QPushButton*    m_toneBtn{nullptr};
    QPushButton*    m_quinBtn{nullptr};   // Quindar tones (#2262)
    bool            m_quinActive{false};  // signal-driven flash state
    ClientCompKnob* m_trim{nullptr};
    QWidget*        m_meter{nullptr};
    QLabel*         m_pkValue{nullptr};
    QLabel*         m_rmsValue{nullptr};
    QLabel*         m_grValue{nullptr};
    QLabel*         m_crestValue{nullptr};
    QWidget*        m_ovrLed{nullptr};
    QLabel*         m_limitLed{nullptr};
    QLabel*         m_activityLbl{nullptr};
    QTimer*         m_meterTimer{nullptr};
    QElapsedTimer   m_animClock;

    // Project-canonical MeterSmoother ballistics — 30 ms attack /
    // 180 ms release at 120 Hz polling.  Targets normalised to [0, 1]
    // via dbToRatio(); m_*Db members hold the post-smoother dB values
    // for label readouts and HorizMeter rendering.
    MeterSmoother m_inPeakSmooth;
    MeterSmoother m_outPeakSmooth;
    MeterSmoother m_outRmsSmooth;
    MeterSmoother m_grSmooth;        // GR is positive-going; target = -gr/30 clamped to [0, 1]
    float    m_inPeakDb{-120.0f};
    float    m_outPeakDb{-120.0f};
    float    m_outRmsDb{-120.0f};
    float    m_grDb{0.0f};
    bool     m_active{false};
    quint64  m_lastClipCount{0};
    qint64   m_ovrLatchUntilMs{0};
    bool     m_limitFlashOn{false};   // toggled each tick while active
};

} // namespace MasterSDR
