#pragma once

#include "core/PcmFrame.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include "core/backends/sim/NoiseMixer.h"

namespace AetherSDR {

// SimSignalSource — the demo radio's synthetic RX engine (NoiseMixer, frame
// pacing, audio + spectrum emission) on a worker thread, so generation never
// competes with the GUI thread's painting (#4878, #502).
//
// Threading contract: construct anywhere, moveToThread(worker), then touch it
// ONLY through queued calls; every public slot assumes it runs on the worker.
// startSession()/stop() are slots so the member QTimer starts on its own thread.
// Signals are emitted on the worker and cross back queued.
class SimSignalSource : public QObject {
    Q_OBJECT

public:
    explicit SimSignalSource(QObject* parent = nullptr);

    // One spectrum row per this many audio frames (~21 rows/s at 128/24k).
    static constexpr int kSpectrumRowEveryNFrames = 9;

public slots:
    void startSession(quint64 session);
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
    void audioFrameReady(const AetherSDR::PcmFrame& stereo);
    void sliceAudioFrameReady(int sliceId, const AetherSDR::PcmFrame& stereo);
    // `session` is the value startSession() was given, so the backend can
    // drop a row a previous session queued, as the PcmFrame epoch does for
    // audio (#6084).
    void spectrumFrameReady(int panId, quint64 session, const QByteArray& bins);

private:
    friend struct SimReceiveContractTestAccess;
    PcmProducer m_speakerPcm;
    PcmProducer m_slicePcm;
    quint64 m_session{0};   // stamps spectrum rows; set by startSession()
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
