#pragma once

#include "core/backends/IRadioBackend.h"
#include "core/dsp/WdspChannel.h"

#include <QElapsedTimer>
#include <QString>
#include <QThread>
#include <QTimer>

#include "core/backends/hl2/Hl2DbReference.h"
#include "core/backends/hl2/MetisProtocol.h"   // Hl2Telemetry

namespace AetherSDR::hl2 {

class MetisClient;
class Hl2RxDsp;
class Hl2TxDsp;

// IRadioBackend implementation for the Hermes-Lite 2 (HPSDR Protocol 1, raw IQ).
// Owns a MetisClient (UDP wire) and an Hl2RxDsp (demod + panadapter) and maps the
// neutral seam verbs/signals onto them. This is the first backend that owns an
// engine-side DSP chain (RFC §5.5) rather than decoding a cooked stream.
//
// THIS BACKEND CAN KEY THE RADIO. capabilities().canTransmit reports transmit
// AVAILABILITY rather than a constant false: an interactive run may transmit,
// and an automation run defers to the bridge's own TX gate. MetisClient refuses
// independently at the wire, so neither gate is trusted as the only one.
//
// The wire and both DSP chains run on a dedicated I/O thread. That is not only
// about keeping WDSP off the UI: this backend paces EP2, and the gateware
// watchdog halts the stream if EP2 stops arriving.
class Hl2Backend : public IRadioBackend {
    Q_OBJECT

public:
    explicit Hl2Backend(QObject* parent = nullptr);
    ~Hl2Backend() override;

    RadioCapabilities capabilities() const override;

    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override;

    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setPanCenter(const QString& panId, double hz) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanRfGain(const QString& panId, int gainDb) override;

private:
    // The actual span change, after the throttle above has settled.
    void applyPanBandwidth(double hz);

public:
    void setPanFrameRate(const QString& panId, int fps) override;
    void setKeying(bool key) override;
    void submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz) override;
    void setTxPower(int percent) override;
    void setTune(bool on) override;
    void setTxFrequency(double hz);
    void setTxDriveLevel(int level);
    // Baseband TX test tone, offsetHz from the carrier, amplitude 0..1.
    // Opt-in only — never enabled by a default.
    void setTxTestTone(double offsetHz, double amplitude);

    void invokeExtension(const QString& ns, const QString& verb, quint64 requestId,
                         const QVariant& arg) override;

    HealthSnapshot healthSnapshot() const override;

private:
    // Re-select the companion filter board's band filter for the current slice
    // frequency. Idempotent and change-gated, so it is safe to call from every
    // path that can move the dial.
    void applyBandFilter(const char* reason);

    void emitSliceState();   // sliceChanged(delta) from current freq/mode/filter
    void emitPanState();     // panCenterBandwidthChanged from freq + sample rate
    void pushInitialState();
    void defineMeters();
    void publishTelemetry(const Hl2Telemetry& t);
    static double temperatureCelsius(int raw);
    // Uncalibrated directional-coupler counts -> watts. See the table in the
    // .cpp for what this curve is and, more importantly, what it is not.
    static double directionalWatts(int raw);
    // Watts -> dBm for the meter seam, floored so 0 W does not become -inf.
    static double wattsToDbm(double watts);

    MetisClient* m_metis = nullptr;
    Hl2RxDsp* m_dsp = nullptr;
    Hl2TxDsp* m_txDsp = nullptr;
    bool m_connected = false;

    // Authoritative RX state (HL2 has no status wire echoing it back).
    // The slice's tuned frequency, and — separately — where the DDC's NCO sits.
    // These were one value, which nailed the slice to the centre of the
    // panadapter: every tune moved the NCO, so the pan centre moved with it
    // and the display re-centred under the operator on every click. They are
    // now independent, with the slice tuned inside the passband by a WDSP
    // shift and the NCO moved only when the target would leave the window.
    double m_rxFreqHz = 10'000'000.0;   // slice
    double m_ncoHz    = 10'000'000.0;   // DDC / pan centre
    // The DDC rate, which IS the panadapter span (emitPanState).
    //
    // Defaults to the NARROWEST the hardware offers, and connectRadio then
    // replaces it with whatever span the operator last chose (Hl2Settings).
    // The widest costs ~8x the narrowest in both directions -- 25.2 vs 3.1 Mbps
    // sustained UDP, 3048 vs 381 packets/second, and 8x the samples through
    // WDSP's decimation front end -- so it is opted into, never imposed at
    // connect on an operator who may be on wifi or a host that cannot carry it.
    int m_sampleRateHz = 48000;
    // Zoom-sweep throttle for setPanBandwidth.
    //
    // Unlike a centre drag, which is cheap to forward, a span change is a
    // BLOCKING WDSP reconfigure plus a settings write. A drag delivers commands
    // every ~33 ms, and a sweep from the narrowest span to the widest crosses
    // every intermediate rate — so the operator paid for two full rebuilds whose
    // results were discarded before either was ever seen. Worse, those rebuilds
    // run on the thread that paces EP2, and the gateware watchdog halts the
    // stream if EP2 stops arriving.
    //
    // Leading edge applies immediately, so a single discrete zoom step still
    // responds at once; anything arriving inside the cooldown is coalesced and
    // the last one applied when it expires. (#4470)
    static constexpr int kBandwidthThrottleMs = 150;
    QTimer* m_bandwidthThrottle = nullptr;
    double m_pendingBandwidthHz = 0.0;   // 0 = nothing coalesced
    QString m_mode = QStringLiteral("USB");
    int m_filterLowHz = 150;
    int m_filterHighHz = 3000;
    int m_lnaGainDb = 20;
    // Last J16 open-collector filter byte commanded. 0xFF is "nothing sent yet"
    // rather than a real selection — kOcNone (0x00) is a legitimate value
    // meaning "every relay released", so it cannot double as the sentinel.
    int m_ocFilterByte = 0xFF;
    // Owns the LNA gain <-> dBm coupling so a gain change cannot move the trace.
    Hl2DbReference m_dbRef;

    // The wire and the DSP both live here, off the GUI thread. See MetisClient's
    // header for why the EP2 pacer in particular must not share a thread with
    // the UI. Owned by this object; joined in the destructor.
    QThread* m_ioThread = nullptr;

    // Process-wide transmit availability, decided once at construction:
    // interactive runs may transmit; automation runs defer to the bridge's
    // AETHER_AUTOMATION_ALLOW_TX gate. Mirrored into MetisClient, which refuses
    // independently at the wire.
    bool m_txAllowed = false;
    Hl2Telemetry m_telemetry;
    // Cumulative EP6 sequence gaps, mirrored onto THIS thread from
    // MetisClient::dropsUpdated. Deliberately a copy rather than a call into
    // MetisClient::droppedPackets(): that object lives on the I/O thread, and
    // healthSnapshot() is read from the GUI thread.
    quint64 m_drops = 0;
    bool m_adcOverload = false;
    bool m_keyed = false;
    bool m_tuning = false;
    bool m_toneFromTune = false;
    // Tune-carrier amplitude, full scale into the modulator. Actual radiated
    // power is governed by the TX drive register, which is where an operator
    // sets it; scaling here as well would make the power control non-linear for
    // no reason.
    static constexpr double kTuneCarrierAmplitude = 1.0;
    int m_lastFwdRaw = -1;

    // ---- Meter pacing / ballistics ----
    //
    // WDSP hands us a signal-strength reading once per demodulated block, which
    // at 24 kHz output is ~47 a second and scales with the span. Every one of
    // them crossed the thread boundary into MeterModel and repainted the
    // S-meter, so the needle was being driven far faster than it can be read
    // and far faster than a Flex drives the same widget.
    //
    // Two separate things fix that and they are NOT interchangeable:
    //   - the RATE gate below decides how often a value is published;
    //   - the EMA decides what value gets published when it is.
    // Dropping samples without smoothing would alias — the meter would show
    // whichever instant happened to land on the tick.
    //
    // 100 ms is the cadence MetisClient already publishes radio telemetry at
    // (kTelemetryMinIntervalMs), so every HL2 meter now updates on one clock.
    static constexpr qint64 kMeterPublishIntervalMs = 100;
    // Flex's own meter ballistics, from MeterModel's forward-power smoothing:
    // fast attack so a peak is not missed, slow decay so the needle settles.
    // Reused rather than re-invented so an operator moving between a Flex and
    // an HL2 sees meters that behave the same way.
    static constexpr double kMeterAttackAlpha = 0.5;
    static constexpr double kMeterDecayAlpha  = 0.15;
    QElapsedTimer m_sMeterClock;
    double m_sMeterDbm = 0.0;
    bool   m_haveSMeter = false;
    // PA temperature rides the 10 Hz telemetry, so it needs no rate gate of its
    // own — but the instrumentation ADC's low bits are noisy enough that the
    // displayed value flickered by a degree at rest. Same EMA, symmetric:
    // heating and cooling are both slow and neither deserves a fast attack.
    static constexpr double kPaTempAlpha = 0.2;
    double m_paTempC = 0.0;
    bool   m_havePaTemp = false;
    // Authoritative AGC state, mirroring the DSP defaults in Hl2RxDsp::Config so
    // the first sliceChanged reports what WDSP was actually opened with.
    QString m_agcMode = QStringLiteral("med");
    int m_agcThresholdDb = 65;

    // Fraction of the half-span the slice may occupy before the NCO re-centres.
    // 0.8 leaves the outer 20% of each side for filter roll-off.
    // Slice AGC threshold (0..100) -> WDSP gain ceiling in dB. 0.6 spans
    // 0..60 dB; see the measurement in setSliceAgc().
    static constexpr double kAgcCeilingDbPerUnit = 0.6;
    static constexpr double kUsablePassbandFraction = 0.8;
    static constexpr int kSliceId = 0;
    static constexpr const char* kPanId = "hl2";

    // AD9866 LNA gain limits, in dB. These are the range ccRxGain() encodes
    // (C4 = 0x40 | (dB + 12), a 6-bit field), so they are the register's own
    // limits rather than a policy choice — clamping anywhere else would let a
    // value be silently truncated on the wire instead of stopping at the end of
    // the slider's travel.
    static constexpr int kLnaGainMinDb  = -12;
    static constexpr int kLnaGainMaxDb  = 48;
    static constexpr int kLnaGainStepDb = 1;
};

}  // namespace AetherSDR::hl2
