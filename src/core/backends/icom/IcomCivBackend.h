#pragma once

#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QVariantMap>
#include <QString>
#include <QVariantList>
#include <QElapsedTimer>

#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/backends/IRadioBackend.h"
#include "core/backends/icom/CivCodec.h"
#include "core/backends/icom/IcomCivScheduler.h"
#include "core/backends/icom/IcomMeters.h"
#include "core/backends/icom/IcomControls.h"   // the control registry scrubDrive walks
#include "core/backends/icom/IcomModels.h"
#include "core/backends/icom/IcomScope.h"
#include "core/backends/icom/IcomSession.h"

class QTimer;

namespace AetherSDR {
// Lives in AetherSDR, not the global namespace — declaring it globally makes
// the member below an incomplete type that only fails at the point of use.
class Resampler;
}  // namespace AetherSDR

namespace AetherSDR::icom {

// The IRadioBackend implementor for Icom networked radios.
//
// Everything below this class is transport and codec; everything here is
// translation into AetherSDR's neutral seam. The split is what lets the whole
// session be tested against a fake radio without constructing a backend, and
// what will let a future local-serial transport reuse the same translation.
//
// THREE THINGS THIS BACKEND IS NOT, stated up front because each one is a
// tempting wrong assumption:
//
//   * It does NOT modulate on the host. The radio owns the modulator, so
//     hostModulates is false and submitTxAudio ships PCM rather than baseband
//     IQ. (Contrast the HL2, where the opposite is true of both.)
//   * It does NOT produce IQ, and cannot. No networked Icom emits samples.
//     hasDaxStreams is false and there is no IQ path to add later.
//   * It does NOT own the radio's operating state. An Icom remembers its own
//     frequency, mode and filter across power cycles and reports them on
//     request, so clientSettingsDomains is EMPTY and this backend must never
//     push a restored state (Constitution II/III).
class IcomCivBackend : public IRadioBackend {
    Q_OBJECT

public:
    explicit IcomCivBackend(QObject* parent = nullptr);
    ~IcomCivBackend() override;

    // ---- identity & capability ----
    [[nodiscard]] RadioCapabilities capabilities() const override;

    // TRUE. Demodulated audio arrives over the seam, not through a Flex
    // PanadapterStream — this is the gate the RX-audio wiring keys off.
    [[nodiscard]] bool ownsRxAudio() const override { return true; }

    // ---- lifecycle ----
    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    [[nodiscard]] bool isConnected() const override;

    // ---- intents DOWN ----
    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setPanCenter(const QString& panId, double hz,
                      PanCenterIntent intent) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanRfGain(const QString& panId, int gainDb) override;
    void setPanPreamp(const QString& panId, int step) override;
    void setPanAttenuator(const QString& panId, int step) override;
    void setSliceRxAntenna(int sliceId, const QString& antenna) override;
    void setKeying(bool key) override;
    void setTune(bool on, int tunePowerPercent = -1) override;
    void setTxPower(int percent) override;
    void setSpeechProcessor(bool on, int level) override;
    void setMicGain(int gainPercent) override;
    void setTxAudioMonitor(bool on) override;
    void setTxMonitor(bool on, int level) override;
    void setSliceNoiseReduction(int sliceId, bool on, int level) override;
    void setSliceNoiseBlanker(int sliceId, bool on, int level) override;
    void setSliceAutoNotch(int sliceId, bool on) override;
    void setSliceManualNotch(int sliceId, bool on, int position) override;
    void setSliceSquelch(int sliceId, bool on, int level) override;
    void setSliceAudioGain(int sliceId, int gainPercent) override;
    void setVox(bool on, int level, int delayMs) override;
    void setAtu(bool start) override;
    void setRitEnabled(bool on) override;
    void setXitEnabled(bool on) override;
    void setRitOffset(int hz) override;
    void submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz,
                       bool clientLeveled) override;
    void invokeExtension(const QString& ns, const QString& verb, quint64 requestId,
                         const QVariant& arg = {}) override;

    // ---- diagnostics ----
    [[nodiscard]] HealthSnapshot healthSnapshot() const override;

    // ---- the control registry, as the bridge sees it ----------------------
    //
    // controlMap() is the DECLARED truth: IcomControls.h joined with what this
    // backend can observe about itself — whether a control was read at connect,
    // whether its reply has been seen, and whether anything has been sent to it
    // this session. Cheap, read-only, and safe with no radio attached.
    //
    // controlScrub() is the CHECK. For every settable control it drives the seam
    // and verifies that the exact frame reached either the wire or the scheduler.
    // Because the bridge request is synchronous while CI-V dispatch is not, a
    // scheduled result is completed by waiting for `civ scheduler` to drain
    // without a timeout. `filter` narrows it to one id or one plane; empty
    // scrubs everything safe.
    //
    // NEITHER KEYS THE TRANSMITTER. The scrub deliberately excludes ptt, tuner
    // and power: two of them transmit and the third cannot be undone over WiFi.
    [[nodiscard]] QVariantList controlMap() const;
    [[nodiscard]] QVariantList meterMap() const;
    [[nodiscard]] QVariantMap controlScrub(const QString& filter);
    // Returns false when the row cannot be re-asserted safely — the scrub's
    // third outcome, distinct from linked and from broken.
    bool scrubDrive(const icom::ControlSpec& spec);
    [[nodiscard]] LinkStats linkStats() const override;

    // Which meters the UI is currently showing. Metering shares the CI-V stream
    // with tuning, so an unwatched meter's round trip is pure contention — see
    // MeterPoller. Public so the seam can drive it once a verb exists; until
    // then the backend polls a small default set.
    void setMeterVisible(MeterId id, bool visible);

    // The model this backend resolved from CI-V 0x19 0x00, or the conservative
    // fallback until the radio answers.
    [[nodiscard]] const IcomModel& model() const noexcept { return *m_model; }

private slots:
    void onSessionConnected(const QString& deviceName);
    void onSessionDisconnected(const QString& reason);
    void onCivFrame(const AetherSDR::icom::CivFrame& frame);
    void onAudio(const std::vector<float>& mono);
    void onMeterTick();
    void onLinkTick();

private:
    void publishCapabilities();
    // Publish the scope's dBm axis, derived from the SAME ScopeCalibration that
    // toDbm() decodes with. Call whenever anything it depends on changes — at
    // connect, and on every reference-level change.
    void publishScopeDbmRange();
    // The neutral mode string for whatever CivMode the radio is in, or an
    // empty string for a mode with no neutral equivalent (D-STAR).
    // Everything that reports a mode to the models needs this.
    QString currentNeutralMode() const;
    // The mode name the FILTER LADDER is keyed on. Differs from the neutral one
    // only where a radio mode has no neutral equivalent but does have its own
    // IF widths — RTTY today. See the definition.
    QString currentLadderMode() const;
    // Publish the current mode, its passband and the filter ladder from
    // m_mode/m_dataMode/m_filter. SHARED, because the mode arrives on two
    // different commands — 01/04 carry mode and slot, 26 carries mode, DATA and
    // slot — and a 26 that did not republish would decode the DATA flag into a
    // mode indicator that never changed.
    void publishModeState();
    void publishMeterDefs();
    void sendUserCommand(const std::vector<std::uint8_t>& frame);
    void queueRead(const std::vector<std::uint8_t>& frame, const std::string& key,
                   IcomCivScheduler::Priority priority, qint64 notBeforeMs = 0);
    void queueWrite(const std::vector<std::uint8_t>& frame, const std::string& key,
                    IcomCivScheduler::Priority priority, bool supersedes = true);
    void queueEmergencyWriteNoReply(const std::vector<std::uint8_t>& frame,
                                    const std::string& key);
    void pumpCiv(qint64 nowMs);
    // Monotonic milliseconds since construction. THE clock for this backend:
    // every interval here (dispatch slot, reply timeout, poll period, stall
    // threshold, trace age) is measured against it, and none of them survives
    // a wall-clock step. See the constructor.
    [[nodiscard]] qint64 nowMs() const;
    [[nodiscard]] std::string semanticKey(std::span<const std::uint8_t> frame) const;
    [[nodiscard]] std::optional<std::vector<std::uint8_t>>
        confirmationFor(std::span<const std::uint8_t> frame) const;
    [[nodiscard]] QVariantMap schedulerDiagnostics() const;
    void serviceSchedulerWaiters(qint64 nowMs);
    void applyScopeStartup();
    // The connect-edge read burst, lifted out of onSessionConnected UNCHANGED.
    //
    // It is a function only so the rare unknown-model path can defer it until
    // the address is known, and so a retarget can re-issue it — those reads went
    // to an address nobody was listening on, so they returned nothing and
    // re-sending them is both correct and the only way to recover the session.
    //
    // NOT a place to re-pace or re-order anything. RFC #4983 names this burst's
    // bunching as a suspected cause of an unrecoverable CI-V stall; restructuring
    // it belongs to that scheduler work, not here.
    void sendConnectReadBurst();
    // Adopt (or refuse) the address the radio reported in its 0x19 0x00 reply.
    void adoptReportedCivAddress(std::uint8_t reported);
    [[nodiscard]] int sliceId() const noexcept { return 0; }
    [[nodiscard]] QString panId() const { return QStringLiteral("0"); }

    std::unique_ptr<IcomSession> m_session;
    const IcomModel* m_model = nullptr;

    // ---- CI-V address resolution (see IcomSettings::CivSelection) ------------
    //
    // The operator's choice sets WHO WE TALK TO. The 0x19 0x00 reply says WHAT
    // IT IS. Keeping those apart is what lets a typed address select a device on
    // a shared bus while the radio stays authoritative about its own identity.

    // A typed hex address: a device selection, so the wire must not retarget it.
    // A picked model is NOT pinned — it is a shortcut for an address.
    bool m_civAddressPinned = false;
    // The address the session opened with, before any wire adoption. What a
    // two-responder bus falls back TO.
    std::uint8_t m_civSeedAddress = 0;
    // The address adopted from a 0x19 0x00 reply this session, 0 if none yet.
    std::uint8_t m_civReported = 0;
    // Two DIFFERENT addresses answered. Adopt neither — on a bus fronted by
    // Icom's own RS-BA1 server the second responder may be a rotator or an amp,
    // and picking either at random mis-decodes the rest of the session.
    bool m_civAmbiguous = false;
    // Whether sendConnectReadBurst() has already run this session.
    bool m_connectBurstSent = false;
    // The model the RS-BA1 handshake NAMED. Kept separately from m_model because
    // it is the third signal that separates "right radio, changed address" from
    // "wrong radio entirely" — see adoptReportedCivAddress().
    const IcomModel* m_modelByName = nullptr;
    // Bounded, single-shot, never a poll: the unknown-model path waits this long
    // for a broadcast reply before giving up and bursting at the fallback
    // address, so a radio that answers nothing still connects.
    QTimer* m_civDetectTimer = nullptr;
    static constexpr int kCivDetectTimeoutMs = 1000;
    // applyScopeStartup() now has two callers — the connect edge and a late
    // model resolution — and the radio only needs telling once.
    bool m_scopeStarted = false;

    ScopeDecoder m_scope;
    ScopeCalibration m_scopeCal;
    MeterPoller m_meters;
    IcomCivScheduler m_civScheduler;
    QElapsedTimer m_clock;

    // 48 kHz mono from the radio -> 24 kHz interleaved stereo for the engine.
    //
    // BOTH halves of that conversion are load-bearing and neither is optional:
    // the seam's per-slice audio contract is interleaved stereo float32 at
    // 24 kHz (Hl2RxDsp::audioReady says so in its signature, and TciServer's
    // resampler is constructed with a 24000 source rate). Feeding it 48 kHz
    // mono plays back an octave low in one ear, which through TCI means WSJT-X
    // decodes nothing and the spectrum looks half as wide as it is.
    std::unique_ptr<Resampler> m_rxResampler;
    // The mirror of m_rxResampler: the engine's transmit tap runs at 24 kHz and
    // this radio's audio stream at 48. Keyed by source rate so a change in the
    // engine's rate rebuilds it rather than silently resampling from the wrong
    // ratio.
    std::unique_ptr<Resampler> m_txResampler;
    int m_txResamplerFromHz = 0;
    int m_txResamplerToHz = 0;
    // The DEFAULT audio rate, not the only one. 48 kHz 16-bit mono LPCM is
    // 768 kbps in each direction — about 1.5 Mbps of uncompressed UDP for a
    // duplex session, which saturates a marginal 2.4 GHz link and starves the
    // CI-V stream sharing it. `m_audioRateHz` is what the session actually
    // negotiated; everything that resamples must use that, not this.
    static constexpr int kRadioAudioRateHz  = 48000;
    // What a low-bandwidth session asks for. SSB is a 3 kHz passband and FT8 is
    // a single tone, so 16 kHz costs nothing audible and is a THIRD of the
    // traffic. Not lower: 8 kHz starts to audibly dull SSB.
    static constexpr int kLowBandwidthAudioRateHz = 16000;
    int m_audioRateHz = kRadioAudioRateHz;
    static constexpr int kEngineAudioRateHz = 24000;

    QTimer* m_meterTimer = nullptr;
    QTimer* m_linkTimer = nullptr;

    QString m_deviceName;
    std::uint64_t m_frequencyHz = 0;
    CivMode m_mode = CivMode::Usb;
    bool m_dataMode = false;
    bool m_connected = false;
    bool m_keyed = false;
    std::optional<bool> m_pendingPttIntent;
    qint64 m_pendingPttUntilMs = 0;
    bool m_overflow = false;
    double m_vdVolts = 0.0;
    double m_idAmps = 0.0;
    int m_txPowerPercent = 0;
    // Keying can originate at the radio's own PTT, so transmit state is POLLED
    // rather than inferred from our own commands. Slow: it only has to notice a
    // transmission, and it shares the CI-V stream with tuning.
    std::int64_t m_lastPttPollMs = 0;
    static constexpr int kPttPollMs = 250;
    // The scope geometry the RADIO last reported, from its own sweeps. Both pan
    // intents reason against it: a zoom step needs to know which of the eight
    // spans it is leaving, and a centre request needs a truth to snap back to.
    // Zero means no sweep has arrived yet, in which case neither intent acts.
    // Last enable state actually SENT for each radio-side DSP function, so a
    // level change does not re-send the enable.
    //
    // Live testing showed why: the level setter carries the current enable with
    // it (they travel as a pair by design), so "NR on at level 60" arrived as
    // level-then-enable and put 16 40 00 on the wire immediately before
    // 16 40 01 — a real, if brief, disable of the operator's noise reduction,
    // and two frames on a CI-V stream that metering already shares.
    // -1 = unknown, 0 = off, 1 = on.
    int m_nrEnableSent = -1;
    int m_nbEnableSent = -1;
    int m_anfEnableSent = -1;
    int m_mnEnableSent = -1;

    // Which of the three IF filter slots the radio is in (1 = FIL1, the
    // widest). Decoded from the SECOND byte of the mode reply, which this
    // backend used to discard — it is the only way to know, because an
    // IC-705 cannot report a passband in Hz. Kept across mode changes so
    // visiting another mode does not silently reset a narrow filter.
    int m_filter = 1;

    // LAST INTENT PER CONTROL — what we most recently asked the radio for, in
    // the seam's own units. Not a cache of the radio's state: it is what
    // `controls.scrub` re-asserts, so a linkage check can drive every control
    // without moving any of them. A radio that disagrees corrects these through
    // the ordinary decode path.
    int     m_rfGainPercent = 0;
    int     m_preampStep = 0;
    int     m_attenStep = 0;
    int     m_nrLevelPercent = 0;
    int     m_nbLevelPercent = 0;
    int     m_notchPosPercent = 50;
    int     m_squelchPercent = 0;
    int     m_micGainPercent = 0;
    int     m_compLevelPercent = 0;
    bool    m_compEnable = false;
    bool    m_monitorOn = false;
    int     m_monitorLevelPercent = 0;
    QString m_agcMode = QStringLiteral("med");
    int     m_afGainPercent = 0;
    bool    m_voxOn = false;
    int     m_voxLevelPercent = 0;
    int     m_voxDelayMs = 0;
    // -1 = unknown, 0 = off, 1 = on. The dedupe pattern m_nrEnableSent
    // documents: a set is answered with a bare FB, so re-sending an
    // unchanged enable is pure traffic on a stream metering already shares.
    int     m_voxEnableSent = -1;
    int     m_monitorSent = -1;
    bool    m_ritOn = false;
    bool    m_xitOn = false;
    int     m_ritOffsetHz = 0;
    int     m_controlPollPhase = 0;
    bool    m_rxAntennaExternal = false;

    // The radio's MOD Input selection, as last reported (-1 = not yet read).
    //
    // THE SINGLE MOST IMPORTANT SETTING FOR TRANSMIT, and the one nothing else
    // can infer. The radio modulates from ONE source per mode class; if it is
    // not WLAN then every byte of network audio is discarded and the radio
    // transmits its own microphone or nothing at all, at zero forward power,
    // with no error anywhere in the protocol.
    // TUNE composes its own carrier, because on this radio nothing else will.
    //
    // The radio modulates from the audio WE send. Keying in SSB with silence
    // therefore produces no carrier at all — which is why TUNE stopped working
    // the moment MOD Input was corrected to WLAN: before that the radio was
    // modulating ambient room noise from its own microphone, and that happened
    // to be enough for an antenna tuner to see something.
    //
    // The tone REPLACES the outgoing audio inside submitTxAudio rather than
    // being generated on a timer, so its cadence is the transmit callback's
    // cadence and it cannot drift against the stream it is riding.
    bool m_tuning = false;
    int m_preTuneTxPowerPercent = -1;
    double m_tunePhase = 0.0;
    static constexpr double kTuneToneHz = 1500.0;
    // -6 dBFS. Loud enough for a tuner to read instantly, short of the clipping
    // that would splatter a carrier the operator is deliberately leaving up.
    static constexpr float kTuneToneAmplitude = 0.5f;

    int m_dataOffModInput = -1;   // SSB / CW / AM / FM
    int m_dataModInput    = -1;   // data modes (FT8 and friends)
    int m_usbModLevelPercent = -1;
    int m_accessoryModLevelPercent = -1;
    int m_networkModLevelPercent = -1;
    bool m_micGainReported = false;
    std::optional<bool> m_pcAudioEnabled;
    // WHAT THE OPERATOR HAD, so "off" can put it back instead of guessing.
    //
    // DATA OFF MOD is a four- (IC-705) or six-valued (IC-7300MK2) enum the
    // RADIO persists; PC Audio is a two-state button. Writing a fixed MIC on
    // "off" therefore destroys a USB / ACC / MIC+USB selection the operator
    // set on the front panel and never gets it back. Latched from the readback
    // the instant before this client's FIRST write of the session, so the
    // value put back is the radio's own, not one we invented.
    std::optional<int> m_dataOffModRestore;
    QString m_lastModInputWarning;
    void checkModInput();

    std::int64_t m_scopeCentreHz = 0;
    std::int64_t m_scopeSpanHz = 0;

    // A short ring of recent CI-V frames, both directions, for diagnosis.
    //
    // WHY THIS IS HERE AND NOT IN THE LOG. The decisive evidence for a wire-
    // format bug is one frame and its reply — our command echoed back, then FB
    // (accepted) or FA (rejected). Getting at that used to mean relaunching
    // with QT_LOGGING_RULES set, and on a single-client radio every relaunch
    // costs a session timeout, so a three-line diagnosis took three restarts.
    // Kept in the backend it is readable at any moment through one verb.
    //
    // SCOPE SWEEPS ARE EXCLUDED, and that is what makes the ring usable: they
    // are ~500 bytes at 30 Hz and would evict everything interesting within a
    // second. The exclusion is free — onCivFrame returns on them before it
    // reaches the recorder.
    struct CivTraceEntry {
        std::int64_t atMs = 0;
        bool outbound = false;
        bool routine = false;
        QString hex;
    };
    void traceCiv(bool outbound, std::span<const std::uint8_t> frame, bool routine = false);
    [[nodiscard]] QVariantList civTrace(bool includeRoutine) const;
    std::deque<CivTraceEntry> m_civTrace;
    static constexpr std::size_t kCivTraceMax = 200;

    // Which control ids this session has actually SENT and RECEIVED, keyed by
    // the registry's id. Observed truth rather than declared: a row the table
    // claims is wired but that has never been seen on the wire is exactly the
    // half-wired state the table exists to expose.
    QSet<QString> m_controlsSent;
    // Exact set commands admitted by the scheduler. A scrub runs
    // synchronously while CI-V dispatch/reply is intentionally asynchronous,
    // so admission and physical dispatch must be reported separately.
    QSet<QString> m_controlsScheduled;
    QSet<QString> m_controlsSeen;
    // Rows whose scrub mirror holds a REAL value — the radio answered for it,
    // or we commanded it. Deliberately NOT m_controlsSent, which controlScrub()
    // clears per row to detect the wire and so cannot carry "we set this
    // earlier". Without this set the scrub re-asserts a construction default as
    // if it were the operator's setting; see the guard at the top of
    // scrubDrive().
    QSet<QString> m_controlsValueKnown;
    // Every inbound non-sweep frame, matched or not. Distinguishes a silent
    // radio from a registry that matches nothing.
    quint64 m_framesObserved = 0;

    struct SchedulerWaiter {
        quint64 requestId = 0;
        qint64 deadlineMs = 0;
    };
    std::vector<SchedulerWaiter> m_schedulerWaiters;
    quint64 m_schedulerTimeoutsReported = 0;

    // CI-V stall detection. The transport can be healthy while the command
    // plane is dead — see onLinkTick — so these track the command plane alone.
    qint64  m_lastInboundCivAtMs = 0;
    QString m_lastOutboundCiv;      // the last frame we sent, as hex
    qint64  m_lastOutboundCivAtMs = 0;
    bool    m_civStallReported = false;
    // Long enough that a quiet moment is not an alarm — the slowest poll here is
    // 1 s and a user-command guard can defer it — short enough that an operator
    // has not yet had time to wonder why the S-meter stopped.
    static constexpr qint64 kCivStallMs = 5000;
    // Note the id for a frame we are about to send or have just decoded.
    void noteControlSent(std::uint8_t cmd, std::uint8_t sub, bool hasSub);
    void noteControlScheduled(std::uint8_t cmd, std::uint8_t sub, bool hasSub);
    void noteControlSeen(std::uint8_t cmd, std::uint8_t sub, bool hasSub);
    LinkStats m_link;
};

}  // namespace AetherSDR::icom
