#pragma once

#include <QByteArray>
#include <QString>
#include <QTimer>
#include <QVector>

#include "core/backends/IRadioBackend.h"
#include "core/backends/sim/NoiseMixer.h"

namespace AetherSDR {

// SimBackend — a native, in-process synthetic radio (demo mode). It is the
// second IRadioBackend implementor after FlexBackend, but unlike FlexBackend it
// speaks NO vendor wire: there is no socket, no discovery, no VITA-49 framing.
// Because it sits at the IRadioBackend seam, it emits AE's already-normalized
// deltas directly (RadioDelta/SliceDelta/…), so the whole transport layer is
// simply absent. It generates the state a real radio would report.
//
// Status: Phase 1 skeleton (RFC #4288, demo mode). This class currently proves
// the connection lifecycle only — connect → one synthetic radio identity + one
// slice → disconnect/reconnect — with no spectrum/waterfall yet. The signal
// engine (test patterns, meters, tunable multi-slice) is Phase 2 and will be
// ported from flex-sim's generators against flex-sim/PROTOCOL.md.
//
// Design source: nigelfenton/flex-sim (GPL-3.0). This is a clean-room C++
// reimplementation of our own GPL code against our own in-repo spec; no code is
// copied verbatim (Constitution Principle IV).
//
// TX: capabilities().canTransmit is false in this skeleton — a demo radio must
// never appear to be something that can key a transmitter (Principle VI). The
// UI is expected to label the connection unmistakably as a simulator.
class SimBackend : public IRadioBackend {
    Q_OBJECT

public:
    explicit SimBackend(QObject* parent = nullptr);
    ~SimBackend() override;

    // ---- IRadioBackend ----
    RadioCapabilities capabilities() const override;
    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override;
    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setKeying(bool key) override;
    void invokeExtension(const QString& ns, const QString& verb,
                         quint64 requestId, const QVariant& arg = {}) override;

    // Identity advertised in the connect descriptor / radio-list entry. Stable so
    // the UI can label the demo entry and match it back after connect.
    static QString demoModelName();
    static QString demoSerial();

private slots:
    // Frame tick (kFrameLen / kSampleRate, ~5.3 ms): mixes one audio frame and
    // emits it over the seam, plus a periodic spectrum row. Muted while keyed.
    void onAudioTick();

private:
    // Emit the initial synthetic snapshot a freshly-connected radio would report:
    // the radio-global delta (model/nickname/slices) and one active slice on a
    // sensible default frequency/mode. Phase 2 grows this into the pan + meters.
    void emitInitialState();

    // Serialize a mono float frame to the 24 kHz STEREO float32 QByteArray
    // AudioEngine::feedAudioData() expects (each mono sample duplicated L=R).
    static QByteArray toStereoBytes(const QVector<float>& mono);

    // Phase 2b (audio) — the synthesized-RX-audio engine (white/pink/qrn/birdie/…
    // + TNF/ANF notch). onAudioTick() calls m_audio.mixFrame() and emits it over
    // audioFrameReady() as 24 kHz stereo float32 — the format AudioEngine::
    // feedAudioData() consumes, so the demo audio flows straight into AE's NR
    // chain with no DSP change. spectrum() gives the matching panadapter render.
    // Muted while keyed (Principle VI): a demo radio never sounds live on TX.
    NoiseMixer m_audio;
    QTimer     m_audioTimer;         // drives onAudioTick() at the frame rate
    bool       m_keyed{false};       // muted while keyed (Principle VI — never sounds live on TX)
    quint64    m_audioFrames{0};     // frame counter (throttles the spectrum row)

    bool   m_connected{false};
    double m_sliceFreqMhz{14.100};   // default: 20 m, a lively demo band
    QString m_sliceMode{QStringLiteral("USB")};
    int    m_filterLowHz{100};
    int    m_filterHighHz{2900};
    static constexpr int kSliceId = 0;
    static constexpr int kPanId = 0;
};

}  // namespace AetherSDR
