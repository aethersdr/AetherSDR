#pragma once

#include "core/backends/IRadioBackend.h"
#include "core/backends/ft991/Ft991Device.h"

#include <QElapsedTimer>
#include <QString>
#include <QThread>
#include <QTimer>

#include <vector>

namespace AetherSDR::ft991 {

// IRadioBackend implementation for the Yaesu FT-991/FT-991A: CAT over the
// radio's serial port, audio over its USB codec. The radio demodulates and
// modulates internally, so unlike HL2/Colibri there is NO host DSP chain
// below this seam — what the backend owns is the CAT translation, the
// audio plumbing, and the honest mapping of the 4 kHz audio window onto RF
// (the "panadapter is a lens" contract; see the design doc).
//
// ONE slice, ONE pan, fixed: the slice frequency IS the radio's dial. The
// radio persists its own state, so clientSettingsDomains is EMPTY — the
// first radio-authoritative non-Flex backend (Constitution II/III).
//
// TRANSMIT-capable: keying is CAT TX1/TX0 from setKeying()/setTune() only;
// submitTxAudio() plays the engine's processed TX audio into the codec.
// hostModulates=true because what that capability gates — where the TX
// audio comes from — is this host, even though the RF modulator is in the
// radio.
class Ft991Backend : public IRadioBackend {
    Q_OBJECT

public:
    explicit Ft991Backend(QObject* parent = nullptr);
    ~Ft991Backend() override;

    RadioCapabilities capabilities() const override;
    // Demodulated audio arrives over the seam; audio reaches the engine
    // through the RadioModel relay exactly like HL2/Colibri — see
    // MainWindow::backendFeedsEngineDirectly() before "simplifying" either.
    bool ownsRxAudio() const override { return true; }

    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override;

    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setSliceNoiseBlanker(int sliceId, bool on, int level) override;
    void setSliceNoiseReduction(int sliceId, bool on, int level) override;
    void setSliceAutoNotch(int sliceId, bool on) override;
    void setSliceManualNotch(int sliceId, bool on, int position) override;
    void setRitEnabled(bool on) override;
    void setXitEnabled(bool on) override;
    // ONE shared clarifier register — exactly the case setXitOffset's
    // default aliasing was written for, so XIT deliberately inherits it.
    void setRitOffset(int hz) override;
    void setSliceAudioMute(int sliceId, bool mute) override;
    void setSliceAudioGain(int sliceId, int gainPercent) override;
    void setSliceAudioPan(int sliceId, int panPercent) override;
    void setActiveSlice(int sliceId) override;
    void setTxSlice(int sliceId) override;
    void setPanCenter(const QString& panId, double hz,
                      PanCenterIntent intent) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanFrameRate(const QString& panId, int fps) override;
    void setKeying(bool key) override;
    void setTune(bool on, int tunePowerPercent) override;
    void setTxPower(int percent) override;
    void setMicGain(int level) override;
    void submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz) override;
    void invokeExtension(const QString& ns, const QString& verb, quint64 requestId,
                         const QVariant& arg) override;

    HealthSnapshot healthSnapshot() const override;
    LinkStats linkStats() const override;

    static QString familyName() { return QStringLiteral("ft991"); }

private:
    // The seam's pan identifier for the one receiver.
    static QString panId() { return QStringLiteral("ft991-0"); }
    [[nodiscard]] bool isOurPan(const QString& id) const;

    // The pan window is SYMMETRIC about the dial: [dial−span, dial+span]
    // where span is the audio window. The half with no data is emitted at
    // kPadFloorDbm. Symmetric is load-bearing, not cosmetic: every consumer
    // assumes the active slice can sit at the pan centre, and the GUI's
    // slice-follow policy re-centres the pan onto the slice — with the
    // slice pinned to a window EDGE that became a feedback loop walking
    // the dial span/2 per round, through the CAT to the real radio, until
    // the stack overflowed (see the depth guard below, which caught it).
    // centre == dial makes that re-centre request the identity.
    enum class Sideband { High, Low, Both };
    [[nodiscard]] Sideband sideband() const;

    // Hand the current slice passband to the device's host-side biquad
    // chain (queued) — the one place the filter handles become audible.
    void pushPassbandToDevice();
    // Hand the clarifier triple to the device (queued). One register, two
    // enables — so every clarifier verb funnels through here.
    void pushClarifier();

    // Reflect the RADIO's filter width (SH/NA polls) into the slice's
    // displayed passband: anchored per mode family, signed per sideband.
    // Never sends anything back down — pure reflection.
    void applyRadioWidth(int widthHz);

    void emitSliceState();
    void emitPanState();
    void emitTransmitState();
    void defineMeters();
    void publishLinkStats();
    void routeAudio(const std::vector<float>& pcm);
    [[nodiscard]] double sMeterRawToDbm(int raw) const;

    // ---- objects on the I/O thread ----
    Ft991Device* m_device = nullptr;
    QThread* m_ioThread = nullptr;

    // ---- mirrored radio state (the CAT polls are the authority) ----
    bool m_connected = false;
    double m_dialHz = 14'074'000.0;
    QString m_mode = QStringLiteral("USB");
    int m_filterLowHz = 150;
    int m_filterHighHz = 3000;
    QString m_agcMode = QStringLiteral("med");
    int m_agcThresholdDb = 65;      // display-side only; no CAT threshold
    bool m_moxRequested = false;    // OUR keying intent
    bool m_mox = false;             // what the radio reports (TX != 0)
    bool m_tuneActive = false;
    int m_rfPowerWatts = 100;
    int m_preTunePowerWatts = -1;
    int m_micGainPercent = 100;
    bool m_audioMuted = false;
    float m_audioGain = 1.0f;
    int m_audioPanPercent = 50;

    // Radio-side DSP mirror (CAT polls are the authority). Levels are kept
    // in the slice's own 0..100 scale; the coarser radio scales (NL 0..10,
    // RL 1..15) are mapped at the verb/echo boundary, and an echo only
    // rewrites the UI value when it maps to a DIFFERENT radio step — so a
    // slider at 47 is not yanked to 50 by its own echo.
    bool m_nbOn = false;
    int m_nbLevelUi = 50;
    bool m_nrOn = false;
    int m_nrLevelUi = 50;
    bool m_anfOn = false;
    int m_radioShIndex = -1;
    bool m_radioNarrow = false;
    bool m_manualNotchOn = false;
    int m_manualNotchHz = 0;
    // Clarifier mirror. ONE offset on this radio (see Ft991Cat); the seam
    // already models that shape, so nothing is collapsed here.
    bool m_ritOn = false;
    bool m_xitOn = false;
    int m_clarifierHz = 0;
    // Manual notch position as the seam means it: 0..100 across the
    // passband. Converted to the radio's audio Hz at the boundary.
    int m_manualNotchPos = 50;

    // Where the reflected passband anchors, per mode family (audio Hz).
    // SSB/DATA centre their DSP width near 1500; CW near the sidetone.
    static constexpr int kSsbWidthAnchorHz = 1500;
    static constexpr int kCwWidthAnchorHz = 700;
    static constexpr int kMinFilterEdgeHz = 50;
    double m_coveredSpanHz = 4000.0;
    Ft991Device::OpenInfo m_openInfo;
    QString m_portName;
    int m_baudRate = 38400;

    // Re-entrancy depth across the tune verbs. The lens couples the pan
    // centre rigidly to the dial, and every emission here is applied
    // synchronously by RadioModel — so a GUI policy that answers a pan move
    // with another pan move can recurse through this backend without a
    // single queued hop in between (stack overflow, not a hang). The depth
    // guard truncates any such cycle and names it in the log; the
    // change-gating in the verbs keeps the guard from ever firing in
    // healthy operation.
    int m_tuneVerbDepth = 0;
    static constexpr int kMaxTuneVerbDepth = 8;

    // S-meter ballistics (shared numbers with Colibri/HL2/Flex).
    QElapsedTimer m_sMeterClock;
    double m_sMeterDbm = 0.0;
    bool m_haveSMeter = false;

    std::vector<float> m_audioScratch;

    QTimer* m_linkStatsTimer = nullptr;
    quint64 m_linkBlocksAtLastTick = 0;
    static constexpr int kLinkStatsIntervalMs = 1000;

    // The audio window the lens covers, Hz. 4 kHz holds the widest SSB
    // filter (3.2 kHz) with margin; the honest covered span comes back from
    // the device (bin rounding) in OpenInfo.
    static constexpr double kAudioSpanHz = 4000.0;

    // Displayed full-scale estimate, dBm at the top of the codec's range.
    // UNCALIBRATED, and doubly so here: the radio's own AGC sits in front
    // of the codec (see the design doc). One constant so nothing the client
    // does moves the trace.
    static constexpr double kFullScaleDbm = -10.0;

    // TX meter scaling, raw 0..255, both UNCALIBRATED linear estimates
    // (the CAT manual publishes no calibration): PO full scale = the HF
    // 100 W rating; SWR spans 1.0..4.0 across the meter.
    static constexpr double kPoMeterFullScaleWatts = 100.0;
    static constexpr double kSwrMeterSpanPerCount = 3.0 / 255.0;

    // S-meter raw 0..255 -> displayed dBm, linear and uncalibrated:
    // 0 -> -127 dBm (S0-ish), 255 -> -34 dBm (~S9+40).
    static constexpr double kSMeterFloorDbm = -127.0;
    static constexpr double kSMeterDbPerCount = 93.0 / 255.0;

    // Meter pacing/ballistics — shared numbers with HL2/Flex/Colibri.
    static constexpr qint64 kMeterPublishIntervalMs = 100;
    static constexpr double kMeterAttackAlpha = 0.5;
    static constexpr double kMeterDecayAlpha = 0.15;
    static constexpr int kAudioPanCentre = 50;
};

}  // namespace AetherSDR::ft991
