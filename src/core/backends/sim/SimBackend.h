#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <QVector>

#include "core/backends/IRadioBackend.h"
#include "core/backends/sim/NoiseMixer.h"

class QThread;

namespace AetherSDR {

class RadioConnection;
class PanadapterStream;

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
    // Real demodulated demo audio rides the seam; the PanadapterStream this
    // backend also vends still carries the old shim's synthetic scene.
    bool ownsRxAudio() const override { return true; }
    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override;
    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    // Added to IRadioBackend by the HL2 backend (#4448). The demo has no hardware
    // AGC and no DDC, but it must not silently swallow either intent: it records
    // them and echoes the slice state back so the UI reflects what the operator
    // set (Principle II — the radio is authoritative about its own state).
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setPanCenter(const QString& panId, double hz) override;
    void setKeying(bool key) override;
    void invokeExtension(const QString& ns, const QString& verb,
                         quint64 requestId, const QVariant& arg = {}) override;

    // ---- Demo noise controls (RFC #4288) ----
    // Drive THIS backend's NoiseMixer (m_audio) — the one whose output you hear
    // (onAudioTick → audioFrameReady). The DemoApplet's controls route here.
    // (PanadapterStream has a SECOND, now-unused NoiseMixer from the old shim
    // path; wiring the applet to that one is why the controls did nothing.) Same
    // thread as the applet (SimBackend is not moved to a worker thread), so these
    // are safe direct calls. The birdie "hz" knob is RF-anchored: it sets the
    // carrier's offset above the VFO, matching the standalone tool's behaviour.
    void setDemoNoiseEnabled(const QString& channel, bool on);
    void setDemoNoiseLevel(const QString& channel, double levelDb);
    void setDemoNoiseKnob(const QString& channel, const QString& knob, double value);
    void loadDemoNoisePreset(const QString& presetName);

    // Auto-notch and noise-blanker, applied to the mixer that actually feeds the
    // speaker (m_audio). These used to live only on PanadapterStream's separate
    // NoiseMixer — the inaudible one — so engaging ANF or NB did nothing you
    // could hear. VFO and mode need no entry point here: they already arrive
    // through the IRadioBackend seam (setSliceFrequency/setSliceMode) and now
    // drive the birdie from there.
    void setDemoAnf(bool on);
    void setDemoNb(bool on);

    // Identity advertised in the connect descriptor / radio-list entry. Stable so
    // the UI can label the demo entry and match it back after connect.
    static QString demoModelName();
    static QString demoSerial();
    // The backend-family string for the demo ("sim"), as understood by
    // RadioModel::makeBackend()/setupBackend(). Single definition so the picker,
    // the factory and any test all agree — this is THE selector for which backend
    // a target gets, and a second parallel notion of "is this the demo" is exactly
    // what let the demo run on a FlexBackend.
    static QString familyName();

    // ---- Path B seam (RFC #4288): SimBackend OWNS a RadioConnection and a
    // PanadapterStream in synthetic-demo mode, exactly as FlexBackend owns its
    // real wire objects, and vends them here. RadioModel harvests these as
    // non-owning pointers at the RadioModel.cpp:507 factory and drives them
    // byte-for-byte as it drives FlexBackend's — the synthetic behaviour lives
    // inside RadioConnection::startSyntheticDemoConnect() and
    // PanadapterStream::tickSyntheticDemo() (both already built + verified). This
    // lets the demo reuse AE's entire real connect + spectrum path unchanged. ----
    RadioConnection*  connection() const { return m_connection; }
    PanadapterStream* panStream()  const { return m_panStream; }

private slots:
    // Frame tick (kFrameLen / kSampleRate, ~5.3 ms): mixes one audio frame and
    // emits it over the seam, plus a periodic spectrum row. Muted while keyed.
    void onAudioTick();

private:
    // Emit the initial synthetic snapshot a freshly-connected radio would report:
    // the radio-global delta (model/nickname/slices) and one active slice on a
    // sensible default frequency/mode. Phase 2 grows this into the pan + meters.
    void emitInitialState();

    // ---- Fault-injection harness (RFC #4288 #4 — land in v1) ----
    // Reached via invokeExtension("sim", <fault>, …) — the IRadioBackend vendor-
    // extension seam. Each fault drives AE down a fail-closed path so the
    // regression suite can assert the app degrades safely (Jeremy's CI-testable
    // angle). RX-only is preserved; nothing here can key TX. Faults:
    //   swr <ratio>   — report high reflected power (SWR protection / fold-back)
    //   dropslice     — remove the active slice (slice-gone teardown)
    //   stallscope    — stop emitting spectrum/waterfall (scope-stall watchdog)
    //   disconnect    — force a mid-op disconnect (reconnect/session teardown)
    //   malformed     — emit a garbled status line (parser fail-closed; no retune-to-0)
    //   clear         — cancel active faults, resume normal operation
    // Returns true if the verb was a recognized fault (so invokeExtension can
    // distinguish "handled" from "unknown extension").
    bool applyFault(const QString& fault, const QVariant& arg);
    void injectHighSwr(double ratio);   // define+report a high SWR meter reading
    void injectDropSlice();             // push "slice 0 … removed" wire status
    void injectMalformedStatus();       // push a deliberately garbled wire status line
    void clearFaults();                 // stallscope resume; SWR back to nominal
    // Route a synthetic wire status line through the owned RadioConnection's
    // receive path (queued to its worker thread) so AE decodes it as radio-sent.
    void pushFaultStatus(const QString& line);

    bool   m_scopeStalled{false};   // stallscope: onAudioTick skips the spectrum emit
    double m_lastSwr{1.0};          // last injected SWR (clearFaults resets to nominal)
    static constexpr int kSwrMeterIndex = 90;   // synthetic meter slot for the SWR fault
    // The SWR MeterDef, in one place: injectHighSwr() and clearFaults() both have
    // to define it, and a mismatch between them would route the clear to a
    // different meter than the fault.
    static MeterDef swrMeterDef();

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
    QTimer     m_audioTimer;         // oversampled tick; onAudioTick() paces itself
    // Real-time frame pacing: a 5.333 ms frame is not an integer ms, so a fixed
    // timer interval drifts (over/underproduces → sink drop/starve = wobble).
    // onAudioTick() instead emits whole frames per REAL elapsed time, carrying the
    // sub-frame remainder in m_audioDebtNs so the long-run rate is exactly 24 kHz.
    QElapsedTimer m_audioClock;      // wall clock between ticks (nanoseconds)
    qint64        m_audioDebtNs{0};  // un-emitted elapsed time carried forward
    bool       m_keyed{false};       // muted while keyed (Principle VI — never sounds live on TX)
    quint64    m_audioFrames{0};     // frame counter (throttles the spectrum row)

    bool   m_connected{false};
    double m_sliceFreqMhz{14.100};   // default: 20 m, a lively demo band
    QString m_sliceMode{QStringLiteral("USB")};
    int    m_filterLowHz{100};
    int    m_filterHighHz{2900};
    // Slice AGC (#4448 seam addition). Defaults mirror the slice model's own so
    // the first echo reports what the UI already shows.
    QString m_agcMode{QStringLiteral("med")};
    int     m_agcThresholdDb{65};
    static constexpr int kSliceId = 0;

    // Birdie-vs-VFO geometry, mirroring PanadapterStream's demo behaviour but on
    // the AUDIBLE mixer. The carrier sits a fixed 1200 Hz above the default VFO
    // (matching the ctor's birdie pitch), so tuning across it sweeps the audio
    // pitch down to zero-beat and out the far side.
    bool   m_lsb{false};              // true when the slice mode is a lower sideband
    double m_birdieCarrierMhz{14.100 + 1200.0 / 1.0e6};
    void   updateBirdieFromVfo();     // recompute the birdie pitch from VFO+sideband

public:
    // One waterfall row per this many audio frames (see onAudioTick).
    static constexpr int kSpectrumRowEveryNFrames = 9;
    // The row cadence that follows, in whole ms: 9 × 128 / 24000 s = 48 ms.
    //
    // NOTHING READS THIS, and that is the honest state of it rather than an
    // oversight to be tidied away. It used to be what the synthetic connect put
    // on the wire as `line_duration`, back when that field was believed to be
    // milliseconds; #4606 established that the field is a 1..100 rate, so the
    // wire now carries kWaterfallRate below and this constant is left as the
    // written-down derivation of what the demo actually produces. Keep it in
    // step with kSpectrumRowEveryNFrames — the divergence note under
    // kWaterfallRate is stated against this number.
    static constexpr int kWaterfallRowIntervalMs =
        (kSpectrumRowEveryNFrames * NoiseMixer::kFrameLen * 1000)
        / NoiseMixer::kSampleRate;
    // …and the 1..100 waterfall RATE the synthetic connect declares. The demo
    // already produces rows at its own cadence, so it asks for the top of the
    // control — "as fast as frames arrive" — and RadioModel's pacer leaves the
    // stream ungated. Declaring 48 here was declaring a rate of 48, which under
    // the real semantics (core/WaterfallRate.h) is about 700 ms per row rather
    // than 48, and would have gated the demo down to 1.4 rows/s (#4606).
    //
    // #4425 is why this matters beyond the pacer: the renderer interpolates the
    // waterfall/3D scroll over one row interval, so a declared value that
    // disagrees with the real rate makes every animation cut off part-way and
    // the display jump — until SpectrumWidget's own measurement takes over.
    // Note the residual divergence that leaves: the top of the control seeds the
    // axis at kLocalFastestRowsPerSec (40 ms/row) while the demo really emits
    // every kWaterfallRowIntervalMs (48 ms), so the first ~1 s of scroll is 17%
    // fast before measurement takes over. Visible only on the demo rig, and
    // preferable to re-declaring a rate that would re-gate the stream.
    static constexpr int kWaterfallRate = 100;

private:
    static constexpr int kPanId = 0;
    // Demo pan span (MHz) = 8 kHz. MUST equal both the wire "display pan …
    // bandwidth=0.008" (RadioConnection) AND the spectrum span kAudioSpanHz (8000
    // Hz) in onAudioTick — the spectrum row is the audio scene, and AE stretches
    // it across this pan width, so a mismatch puts the birdie at the wrong
    // frequency / outside the passband. Keep all three in lockstep. (RFC #4288)
    static constexpr double kDemoPanBandwidthMhz = 0.008;

    // ---- Path B owned wire objects (synthetic-demo mode) ----
    // Created on worker threads in the ctor (panStream first, matching
    // FlexBackend's load-bearing #502 order), torn down in the dtor. Non-owning
    // getters connection()/panStream() delegate to them. The demo behaviour is
    // driven entirely through these — SimBackend's own delta emission above is
    // the pure-Path-A skeleton kept for the unit tests, dormant on the live path.
    RadioConnection*  m_connection{nullptr};   // owned; lives on m_connThread
    QThread*          m_connThread{nullptr};
    PanadapterStream* m_panStream{nullptr};    // owned; lives on m_networkThread
    QThread*          m_networkThread{nullptr};
};

}  // namespace AetherSDR
