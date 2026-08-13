#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include "core/backends/sim/NoiseMixer.h"

namespace AetherSDR {

// SimSignalSource — the demo radio's synthetic RX engine, on a worker thread
// (#4878).
//
// This is everything that used to tick inside SimBackend on the GUI thread:
// the NoiseMixer, the frame-pacing timer, and the audio + spectrum emission.
// It was the ONLY RX producer in AetherSDR living on the GUI thread — every
// real backend runs its producers on workers precisely so signal generation
// never competes with paintEvent (#502, AudioEngine; PanadapterStream
// likewise) — and it is also the one producer that GENERATES its data rather
// than merely decoding it. On a software-GL machine the combination (a 3 ms
// Qt::PreciseTimer pinning the event dispatcher plus ~200 allocating signal
// emissions a second) is exactly the multi-second input backlog #4878
// reports.
//
// Threading contract: construct on any thread, moveToThread(worker), then
// touch it ONLY through queued calls — every public slot below assumes it
// runs on the worker. The member QTimer moves with its parent, and start()/
// stop() are slots so the timer is always started from its own thread.
// Signals are emitted from the worker and cross back queued, exactly like
// FlexBackend's wire objects.
//
// The timer is 10 ms COARSE where the GUI-thread version was 3 ms PRECISE:
// on a dedicated worker there is nothing to contend with, the debt-pacing in
// onTick() already absorbs interval jitter (the long-run rate is exactly
// kSampleRate regardless), and a coarse timer lets the kernel batch wakeups
// instead of pinning the poll timeout app-wide.
class SimSignalSource : public QObject {
    Q_OBJECT

public:
    explicit SimSignalSource(QObject* parent = nullptr);

    // One spectrum row per this many audio frames (~21 rows/s at 128/24k).
    static constexpr int kSpectrumRowEveryNFrames = 9;

public slots:
    void start();
    void stop();

    void setKeyed(bool keyed);            // mute while keyed (Principle VI)
    void setScopeStalled(bool stalled);   // the stallscope fault

    // The DemoApplet's noise controls, queued across from the GUI thread.
    void setNoiseEnabled(const QString& channel, bool on);
    void setNoiseLevel(const QString& channel, double levelDb);
    void setNoiseKnob(const QString& channel, const QString& knob, double value);
    void loadPreset(const QString& presetName);
    void setAnf(bool on);
    void setNb(bool on);

    // Birdie-vs-VFO geometry (the audible carrier is RF-anchored, so tuning
    // sweeps its pitch like a real signal). The slice state lives with the
    // mixer because the knob re-anchor and the pitch derivation both need it
    // atomically with the audio it changes.
    void setVfoMhz(double mhz);
    void setLowerSideband(bool lsb);

    // Which pans exist, as backend pan indices in creation order (#4887
    // phase 4 — the demo grew real multi-pan). One spectrum row per live pan
    // every kSpectrumRowEveryNFrames frames, each addressed by its index.
    // The audio stays single-scene: every demo pan is a view of the same
    // antenna, so the mix is computed once and only the emission fans out.
    void setPanIndices(const QList<int>& indices);

signals:
    // 24 kHz stereo float32, the format AudioEngine::feedAudioData() eats.
    void audioFrameReady(const QByteArray& stereo);
    void sliceAudioFrameReady(int sliceId, const QByteArray& stereo);
    void spectrumFrameReady(int panId, const QByteArray& bins);

private:
    void onTick();
    void updateBirdieFromVfo();
    static QByteArray toStereoBytes(const QVector<float>& mono);

    static constexpr int kSliceId = 0;

    NoiseMixer    m_audio;
    QTimer        m_timer;
    QElapsedTimer m_clock;
    qint64        m_debtNs{0};
    quint64       m_frames{0};
    bool          m_keyed{false};
    bool          m_scopeStalled{false};
    QList<int>    m_panIndices{0};   // pan 0 exists from connect

    double m_sliceFreqMhz{14.100};
    bool   m_lsb{false};
    double m_birdieCarrierMhz{14.100 + 1200.0 / 1.0e6};
};

}  // namespace AetherSDR
