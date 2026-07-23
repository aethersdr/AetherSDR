#pragma once

// AetherClock engine: binds a user-chosen RX slice, owns the DAX-hold
// LIFECYCLE for that slice's channel (acquire on start, follow live
// reassignment, release on stop/slice loss), feeds the WWV/WWVB decoders,
// and emits decode + alignment signals.
//
// Radio-seam discipline (EB3): this file never touches the vendor stream
// classes. The wiring layer (GUI applet, tests, any future host) injects a
// DAX-hold provider wrapping the CENTRAL
// PanadapterStream::acquireDaxChannel/releaseDaxChannel(ch,
// DaxConsumer::Clock) registry, and connects the stream's daxAudioReady to
// feedRxAudio(). That keeps the engine above the radio seam and
// source-agnostic: any 24 kHz float32-stereo feed (Flex DAX today, other
// backends tomorrow) drives it through the same two seams.
//
// RX-only by design: no TX path, nothing radio-authoritative persisted, and
// the OS clock is never modified — the engine only READS the host clock
// (through an injectable hook so tests control time).
//
// Threading: thread-agnostic QObject. The creator may move it to a worker
// thread; all cross-object wiring is queued. Decode work runs inside
// feedRxAudio() — the signals carry 1 bit/s, so this is trivially cheap.

#include "ClockAlignmentFrame.h"
#include "ClockDiagnostics.h"
#include "TimeFrameVoter.h"

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QVector>

#include <functional>
#include <memory>

namespace AetherSDR {

class SliceModel;

class AetherClockEngine : public QObject {
    Q_OBJECT
public:
    explicit AetherClockEngine(QObject* parent = nullptr);
    ~AetherClockEngine() override;

    // DAX RX audio sample rate (Hz) — the daxAudioReady contract.
    static constexpr int kSampleRateHz = 24000;

    // Station presets. Listening dial = carrier − 1 kHz, USB.
    static QVector<double> wwvCarrierFrequenciesMHz();  // 2.5, 5, 10, 15, 20
    static double wwvbCarrierFrequencyMHz();            // 0.060
    static double listeningDialMHz(double carrierMHz);  // carrier − 0.001

    // DAX-hold provider — set before start() whenever the audio source is
    // DAX (a missing provider warns and skips hold acquisition, so non-DAX
    // sources can drive feedRxAudio directly). The callbacks wrap the
    // central DAX ownership registry
    // (PanadapterStream::acquireDaxChannel/releaseDaxChannel with
    // DaxConsumer::Clock); the engine drives them with the bound slice's
    // channel and never registers a stream privately.
    void setDaxChannelProvider(std::function<void(int)> acquire,
                               std::function<void(int)> release);

    // Host-clock READ hook (UTC ms since epoch). Defaults to
    // QDateTime::currentMSecsSinceEpoch. Tests inject a fake clock. The
    // engine never writes the OS clock.
    void setHostClock(std::function<qint64()> nowUtcMs);

    // Lock-decay watchdog timeout (ms). Decoder state only advances inside
    // process(); if audio stops arriving (or no second classifies) a
    // Locked/Acquiring state would stick forever. When this window elapses with
    // no classified second while running, the engine demotes the state one step
    // (Locked -> Acquiring -> NoSignal). Test seam; default 10000 ms, values
    // < 50 clamped to 50.
    void setLockDecayTimeoutMs(int ms);

    bool isRunning() const;
    int boundSliceId() const;               // -1 when not bound
    ClockStation configuredStation() const; // station selected at start()
    ClockLockState lockState() const;

    // WS-7 acquisition telemetry: the current diagnostics snapshot, assembled
    // on call from the decoder's read-only accessors plus the engine's
    // classified-seconds ring. The same snapshot is emitted at ~1 Hz via
    // diagnosticsUpdated() while running; this accessor is the test/bridge
    // seam (no event loop required). Default-constructed when not running.
    ClockDiagnostics currentDiagnostics() const;

public slots:
    // Bind `slice` and start decoding. `station` selects the decoder:
    // Wwv (auto-tags Wwvh by tick band) or Wwvb. Acquires the DAX hold on
    // the slice's live daxChannel() through the injected provider, follows
    // daxChannelChanged (acquire-new-before-release-old), and stops
    // gracefully (state → NoSignal, hold released) if the slice is
    // destroyed. Calling start() while running stops first.
    void start(SliceModel* slice, ClockStation station);
    void stop();

    // Convenience tune to listeningDialMHz(carrierMHz) USB; for Wwvb also sets
    // AGC off on that slice. Radio-authoritative state — applied to the live
    // slice only, never persisted; neither binds the slice nor starts the
    // engine. A locked slice refuses the whole preset (all-or-nothing).
    // The two-arg form acts on the BOUND slice (no-op when not bound); the
    // three-arg form acts on any given slice — the applet's Tune-while-stopped
    // path, acting on the strip's selected slice.
    void applyStationPreset(ClockStation station, double carrierMHz);
    void applyStationPreset(SliceModel* slice, ClockStation station,
                            double carrierMHz);

    // PCM ingest — the daxAudioReady payload (float32 interleaved stereo,
    // native-endian, 24 kHz). The wiring layer connects the audio source
    // here; it is also the test-harness seam and the future non-Flex source
    // seam. Samples whose channel differs from the bound slice's live
    // daxChannel() are ignored.
    void feedRxAudio(int channel, const QByteArray& pcm);

signals:
    void runningChanged(bool running);
    void lockStateChanged(AetherSDR::ClockLockState state);
    void lockedChanged(bool locked);
    void stationDetected(AetherSDR::ClockStation station);
    // quality 0-100. offsetMs = decodedUtc − hostUtc at the decoded second
    // edge — POSITIVE means the host clock is BEHIND the broadcast.
    void timeDecoded(const QDateTime& utc, double offsetMs, int quality);
    void alignmentFrame(const AetherSDR::ClockAlignmentFrame& frame);
    // WS-7 telemetry (both additive, read-only w.r.t. decode behavior):
    // ~1 Hz diagnostics while running, and the raw per-frame decode re-emitted
    // instead of dying at the engine boundary (frameConfidence, DUT1/DST/leap
    // feed the debug pane).
    void diagnosticsUpdated(const AetherSDR::ClockDiagnostics& diag);
    void frameDecoded(const AetherSDR::ClockFrameInfo& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace AetherSDR
