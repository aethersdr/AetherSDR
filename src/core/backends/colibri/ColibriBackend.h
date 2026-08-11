#pragma once

#include "core/backends/IRadioBackend.h"
#include "core/dsp/WdspChannel.h"

#include <QElapsedTimer>
#include <QString>
#include <QThread>
#include <QTimer>

#include <vector>

namespace AetherSDR::colibri {

class ColibriDevice;
class ColibriRxDsp;

// IRadioBackend implementation for the Expert Electronics ColibriNANO
// (USB direct-sampling receiver, raw IQ via colibrinano_lib). Owns a
// ColibriDevice (the DLL wire) and a ColibriRxDsp (demod + panadapter) and
// maps the neutral seam verbs/signals onto them — the second backend after
// HL2 that owns an engine-side DSP chain (RFC §5.5), and the first that is
// RECEIVE-ONLY: capabilities().canTransmit is a constant false, setKeying()
// is a guarded no-op, and the engine TX guard above the seam refuses keying
// on the capability alone.
//
// ONE receiver, fixed: the device has a single DDC, so maxSlices and
// maxPanadapters are 1, the pan id is kPanId, the slice id is 0, and HL2's
// four-space index map collapses to constants.
//
// The wire and the DSP run on a dedicated I/O thread. The DLL's RX callback
// arrives on the LIBRARY's own thread and is queued onto the I/O thread at
// the ColibriDevice boundary — see that header for why the extra hop is not
// optional.
class ColibriBackend : public IRadioBackend {
    Q_OBJECT

public:
    explicit ColibriBackend(QObject* parent = nullptr);
    ~ColibriBackend() override;

    RadioCapabilities capabilities() const override;
    // Demodulates in-process (ColibriRxDsp); there is no VITA-49 stream at all.
    // Like HL2, audio reaches the engine through the RadioModel relay — see
    // MainWindow::backendFeedsEngineDirectly() before "simplifying" either.
    bool ownsRxAudio() const override { return true; }

    void connectRadio(const RadioConnectRequest& request) override;
    void applyRestoredState(const RestoredRadioState& state) override;
    RestoredRadioState currentOperatingState() const override;
    void disconnectRadio() override;
    bool isConnected() const override;

    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setSliceAudioMute(int sliceId, bool mute) override;
    void setSliceAudioGain(int sliceId, int gainPercent) override;
    void setSliceAudioPan(int sliceId, int panPercent) override;
    void setActiveSlice(int sliceId) override;
    void setPanCenter(const QString& panId, double hz,
                      PanCenterIntent intent) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanRfGain(const QString& panId, int gainDb) override;
    void setPanFrameRate(const QString& panId, int fps) override;
    // RX-only: keying is refused above the seam (canTransmit=false); this is
    // the RFC §5.5 Q3 guarded no-op.
    void setKeying(bool key) override;
    void invokeExtension(const QString& ns, const QString& verb, quint64 requestId,
                         const QVariant& arg) override;

    HealthSnapshot healthSnapshot() const override;
    LinkStats linkStats() const override;

    static QString familyName() { return QStringLiteral("colibri"); }

private:
    // The seam's pan identifier for the one receiver.
    static QString panId() { return QStringLiteral("colibri-0"); }
    [[nodiscard]] bool isOurPan(const QString& id) const;

    // The actual span change, after the throttle has settled: stop the
    // stream, reconfigure the DSP at the new rate, restart the stream.
    void applyPanBandwidth(double hz);
    void applyPreampDb(double db);

    void emitSliceState();
    void emitPanState();
    void pushInitialState();
    void defineMeters();
    // Publish linkStats() on the fixed cadence the seam promises — a timer
    // here rather than the receive path, so the tick survives the device
    // going silent (which is the case the heartbeat has to detect).
    void publishLinkStats();
    void notifyOperatingStateChanged();
    // Route one demodulated block to the seam: per-slice feed raw, speaker
    // feed with the operator's mute/gain/balance applied.
    void routeAudio(const std::vector<float>& pcm);
    // dBFS -> displayed dBm. Uncalibrated: a constant full-scale estimate
    // minus the preamp gain, which is exactly right in the one way that
    // matters — the trace and meter hold still across a gain change.
    [[nodiscard]] double dbfsToDbm(double dbfs) const;

    // Hand the I/O thread the DSP pointer (or null). Same ownership split as
    // HL2's publishIoDsps: m_rxDsp is GUI-thread state, m_ioDsp is read only
    // on the I/O thread, and the copy is what keeps the sample path lock-free.
    // BLOCKS until the I/O thread has taken the new value.
    void publishIoDsp(ColibriRxDsp* dsp);

    // ---- objects on the I/O thread ----
    ColibriDevice* m_device = nullptr;
    ColibriRxDsp* m_rxDsp = nullptr;    // GUI-thread handle; created per connect
    ColibriRxDsp* m_ioDsp = nullptr;    // I/O THREAD ONLY — the chain the wire feeds
    QThread* m_ioThread = nullptr;

    // ---- authoritative RX state (no status wire echoes anything back) ----
    bool m_connected = false;
    double m_sliceFreqHz = 7'074'000.0;   // slice — FT8 on 40 m, this station's habit
    double m_ncoHz = 7'074'000.0;         // DDC / pan centre
    QString m_mode = QStringLiteral("USB");
    int m_filterLowHz = 150;
    int m_filterHighHz = 3000;
    QString m_agcMode = QStringLiteral("med");
    int m_agcThresholdDb = 65;
    bool m_audioMuted = false;
    float m_audioGain = 1.0f;
    int m_audioPanPercent = 50;
    int m_sampleRateHz = 48000;
    double m_preampDb = 0.0;
    bool m_passbandDerivedThisConnect = false;

    // S-meter ballistics: smooth every sample, publish on the tick (same
    // attack/decay as Flex's MeterModel so the needle behaves the same way).
    QElapsedTimer m_sMeterClock;
    double m_sMeterDbm = 0.0;
    bool m_haveSMeter = false;

    // Audio scratch for the speaker path (gain/balance applied).
    std::vector<float> m_audioScratch;

    // Link stats, published on a fixed cadence.
    QTimer* m_linkStatsTimer = nullptr;
    quint64 m_linkBlocksAtLastTick = 0;
    static constexpr int kLinkStatsIntervalMs = 1000;

    // Zoom-sweep throttle (same shape as HL2 #4470): a span change here is a
    // stream stop/start plus a blocking WDSP rebuild, and a drag delivers
    // requests every ~33 ms.
    static constexpr int kBandwidthThrottleMs = 150;
    QTimer* m_bandwidthThrottle = nullptr;
    double m_pendingBandwidthHz = 0.0;   // 0 = nothing coalesced

    // RFC #4603 restored state.
    bool m_haveRestoredState = false;
    RestoredRadioState m_restoredState;

    // Preamp range, from the library's own documentation (setPream):
    // -31.5..+6 dB in 0.5 dB steps. The seam's gain verb is integer dB, so
    // the advertised range is the integer subset.
    static constexpr int kPreampMinDb = -31;
    static constexpr int kPreampMaxDb = 6;
    static constexpr int kPreampStepDb = 1;

    // Displayed full-scale estimate, dBm at 0 dB preamp. UNCALIBRATED — the
    // honest part is that it is one constant, so gain changes cannot move the
    // trace; the absolute number is an estimate for a direct-sampling ADC
    // with a full-scale input around a few hundred millivolts.
    static constexpr double kFullScaleDbm = -10.0;

    // Meter pacing/ballistics — shared numbers with HL2/Flex so an operator
    // moving between radios sees meters that behave the same way.
    static constexpr qint64 kMeterPublishIntervalMs = 100;
    static constexpr double kMeterAttackAlpha = 0.5;
    static constexpr double kMeterDecayAlpha = 0.15;

    // Fraction of the half-span the slice may occupy before the NCO
    // re-centres; the outer 20% is filter roll-off.
    static constexpr double kUsablePassbandFraction = 0.8;
    // Slice AGC threshold (0..100) -> WDSP gain ceiling in dB (0..90).
    //
    // NOT the HL2's 0.6 (a 0..60 dB span). This receiver delivers IQ tens of
    // dB below an HL2's, so its AGC runs AT the ceiling rather than regulating
    // below it — which makes this constant, not the AGC loop, what sets the
    // audio level. Both ends were measured on 20 m with the preamp at +6 dB,
    // reading the demodulated level off a decoder that reports it:
    //
    //   ceiling 78 dB -> rms 0.61, peak 1.0, samples CLIPPING. A clipped FT8
    //                    slot decodes nothing at all ("zero raw FT rows while
    //                    RX audio is overdriven, clipped=20/240").
    //   ceiling 39 dB -> rms 0.0011, peak 0.004. Inaudible, and equally
    //                    undecodable — 55 dB below the level above.
    //
    // 0.9 puts the slice default of 65 at ~58 dB, landing near rms 0.06
    // (about -24 dBFS), which is the level FT8 decoders are happiest with and
    // roughly the midpoint of the two measurements. The operator's AGC-T
    // slider then spans 0..90 dB, with usable travel either side of that.
    static constexpr double kAgcCeilingDbPerUnit = 0.9;
    static constexpr int kAudioPanCentre = 50;

    // What the library can be tuned to, from its own documentation: DC-ish to
    // 500 MHz through Nyquist zones. The first zone (ADC clock 122.88 MHz) is
    // what the hardware hears without an external filter, and its LPF corner
    // is what we advertise as the honest ceiling.
    static constexpr double kTuningMinHz = 9'000.0;
    static constexpr double kTuningMaxHz = 55'000'000.0;
};

}  // namespace AetherSDR::colibri
