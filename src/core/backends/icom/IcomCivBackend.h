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
#include "core/backends/icom/IcomMemoryCodec.h"
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
    void setTxFilter(int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setPanCenter(const QString& panId, double hz,
                      PanCenterIntent intent) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanRfGain(const QString& panId, int gainDb) override;
    void setPanPreamp(const QString& panId, int step) override;
    void setPanAttenuator(const QString& panId, int step) override;
    void setSliceRxAntenna(int sliceId, const QString& antenna) override;
    void setRadioDialLock(bool locked) override;
    void setKeying(bool key) override;
    void setTune(bool on, int tunePowerPercent = -1) override;
    void setTxPower(int percent) override;
    QString sendCwText(const QString& text) override;
    void abortCwText() override;
    void setCwSpeed(int wpm) override;
    void setCwPitch(int hz) override;
    void setCwBreakIn(bool on) override;
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
    void setSliceFmToneMode(int sliceId, const QString& mode) override;
    void setSliceFmToneValue(int sliceId, double hz) override;
    void setSliceFmToneRxValue(int sliceId, double hz) override;
    void setSliceFmDtcs(int sliceId, int code, bool txReverse,
                        bool rxReverse) override;
    void setSliceRepeaterOffsetDir(int sliceId, const QString& direction) override;
    void setSliceFmRepeaterOffset(int sliceId, double hz) override;
    bool applyMemoryRecallDetails(const MemoryRecallDetails& details) override;
    void refreshMemories(const QString& group) override;
    void setTransmitFrequencyCheck(bool on) override;
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
    [[nodiscard]] QVariantMap profileMap() const;
    [[nodiscard]] QVariantMap repeaterStateMap() const;
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
    void onCivFrame(const AetherSDR::icom::CivFrame& frame,
                    std::uint64_t sessionGeneration);
    void onAudio(const std::vector<float>& mono);
    void onMeterTick();
    void onLinkTick();
    void onTuneAudioTick();

private:
    // Focused access for the generation-gate regression test.  The test must
    // inject a frame carrying an obsolete session generation after the backend
    // has advanced to a replacement session; exercising only the public UDP
    // path cannot make that queued-delivery race deterministic.
    friend struct IcomCivBackendTestAccess;

    void queueTuneAudioFrame();
    [[nodiscard]] int stopTuneProducer();
    void reassertPanPreampWireStep(int step);
    [[nodiscard]] bool tunerSupported() const;
    bool sendTunerCommandIfSupported(bool start);
    bool queueTunerReadIfSupported(std::uint8_t address,
                                   IcomCivScheduler::Priority priority);
    void publishCapabilities();
    // Publish WHAT THIS RADIO IS: the model name, and the band set that follows
    // from it. One call rather than two because they are the same answer — a
    // model whose name reached the UI while its bands did not is how an IC-705
    // ended up with a band menu that had no 2 m or 70 cm button on it (#5041).
    // Emitted from every point that resolves m_model, so the two cannot drift.
    void publishIdentity();
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
    // Re-read the three things that define the passband — the IF width
    // (1A 03) and both Twin PBT positions (14 07 / 14 08).
    //
    // AFTER EVERY MODE AND SLOT CHANGE, not once at connect. All three are
    // stored PER MODE AND PER SLOT in the radio: FIL2 in CW and FIL2 in USB are
    // different widths with different PBT positions, and the radio swaps the
    // lot when the mode changes without announcing any of it. A width read once
    // at connect is correct until the operator's first mode change and silently
    // stale for the rest of the session.
    void requestPassbandState();

    // The passband to draw right now: the radio's own IF width and PBT pair
    // where it has reported them, and the slot ladder's factory default until
    // it has. Signed in SliceModel's convention.
    [[nodiscard]] std::pair<int, int> currentPassbandHz() const;

    // Is the width we hold an answer about the mode/DATA/slot we are in NOW?
    // False means m_ifWidthHz belongs to a context the operator has left, and
    // must not be drawn or trusted until 1A 03 answers again.
    [[nodiscard]] bool passbandWidthIsCurrent() const;

    // Emit ONLY the passband. See the definition — a width or PBT reply has
    // nothing to say about the mode, and saying it anyway republishes a stale
    // one during a front-panel mode change.
    void publishPassband();

    // Which 1A 05 item holds the transmit passband that is actually in circuit:
    // the SSB-DATA slot in a data mode, otherwise whichever of WIDE/MID/NAR
    // 16 58 last reported. Negative when the model has no TBW profile or the
    // radio has not told us which slot is live yet — the caller must then
    // decline the write rather than guess a slot and reshape the wrong one.
    [[nodiscard]] int activeTxBandwidthItem() const;

    // Publish the current mode, its passband and the filter ladder from
    // m_mode/m_dataMode/m_filter. SHARED, because the mode arrives on two
    // different commands — 01/04 carry mode and slot, 26 carries mode, DATA and
    // slot — and a 26 that did not republish would decode the DATA flag into a
    // mode indicator that never changed.
    void publishModeState();
    // Publish THIS MODEL's mode vocabulary onto the slice, so the mode combo
    // offers what the radio actually has instead of the compiled-in FlexRadio
    // list. Emitted on every model resolution, including the one that WITHDRAWS
    // an identity (the ambiguous-bus revert): an empty list is this backend's
    // honest answer for a radio it cannot characterise, and SliceModel carries
    // it. (What the combos do with an empty list is theirs to decide — they keep
    // their last one, #891 — but the model must not go on asserting a vocabulary
    // we have just stopped standing behind.)
    void publishModeList();
    void publishMeterDefs();
    void clearDerivedForwardPower();
    // The receive-only mode gate. True when the radio will not transmit in the
    // mode it is currently in, in which case the caller must NOT key. Warns and
    // puts the transmit indicator back where the radio is. See the definition.
    bool refuseKeyingInReceiveOnlyMode();
    void sendUserCommand(const std::vector<std::uint8_t>& frame);
    void queueRead(const std::vector<std::uint8_t>& frame, const std::string& key,
                   IcomCivScheduler::Priority priority, qint64 notBeforeMs = 0,
                   std::vector<std::uint8_t> replyDataPrefix = {});
    void queueWrite(const std::vector<std::uint8_t>& frame, const std::string& key,
                    IcomCivScheduler::Priority priority, bool supersedes = true,
                    bool coalesce = true);
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
    [[nodiscard]] QVariantList schedulerTransactionTrace(
        std::size_t limit = 32) const;
    [[nodiscard]] QVariantMap incidentSnapshot(const QString& kind,
                                               const QString& reason) const;
    void recordIncident(const QString& kind, const QString& reason);
    enum class SchedulerWaiterOutcome : std::uint8_t {
        Completed,
        TimedOut,
        Failed,
        Cancelled,
    };
    void serviceSchedulerWaiters(qint64 nowMs,
                                 std::optional<SchedulerWaiterOutcome> terminal = std::nullopt,
                                 std::optional<QVariantMap> diagnosticSnapshot = std::nullopt);
    void terminateScheduler(IcomCivScheduler::TerminalOutcome requestOutcome,
                            SchedulerWaiterOutcome waiterOutcome);
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
    int queueMemorySnapshot(const MemoryProfile& profile, int selectedGroup);
    void finishMemoryRefresh(bool success);
    void finishMemoryRefreshWhenDrained(quint64 generation);
    void publishExtendedRepeaterState();
    // Adopt (or refuse) the address the radio reported in its 0x19 0x00 reply.
    void adoptReportedCivAddress(std::uint8_t reported);
    [[nodiscard]] int sliceId() const noexcept { return 0; }
    [[nodiscard]] QString panId() const { return QStringLiteral("0"); }

    std::unique_ptr<IcomSession> m_session;
    std::uint64_t m_sessionGeneration = 0;
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
    bool m_memoryRefreshActive = false;
    quint64 m_memoryRefreshGeneration = 0;
    QSet<int> m_memoryRefreshReplies;
    int m_memoryRefreshTotal = 0;
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
    QTimer* m_tuneTimer = nullptr;

    QString m_deviceName;
    std::uint64_t m_frequencyHz = 0;
    CivMode m_mode = CivMode::Usb;
    bool m_dataMode = false;
    bool m_connected = false;
    bool m_keyed = false;
    bool m_transmitFrequencyCheck = false;
    // Set before an XFC ON enters the scheduler and cleared only by radio
    // readback of OFF (or completed teardown). Capability may change while a
    // command is in flight, but the obligation to release the radio may not.
    bool m_xfcReleaseRequired = false;
    std::optional<bool> m_pendingPttIntent;
    qint64 m_pendingPttUntilMs = 0;
    bool m_pttIncidentReported = false;
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

    // THE WIDTH THAT SLOT ACTUALLY HOLDS, in Hz, from 1A 03 — not the factory
    // default the slot number used to be turned into.
    //
    // ZERO MEANS UNKNOWN, and that distinction is the whole point. An operator
    // who redefined FIL1 to 2.8 kHz in the SET menu got a passband drawn at
    // 3.0 kHz and a button labelled 3.0k, with nothing anywhere saying the
    // number was a guess. Where this is zero the backend falls back to the slot
    // ladder exactly as before; where it is set, it is the radio's own answer
    // and it wins. FM/DV/WFM have no settable width at all and stay zero
    // forever, which is correct rather than missing.
    int m_ifWidthHz = 0;

    // WHICH CONTEXT THAT WIDTH WAS READ FOR — the mode, DATA flag and slot in
    // force when 1A 03 answered.
    //
    // THE RADIO HOLDS A SEPARATE WIDTH FOR EVERY COMBINATION, so a width read
    // in AM says nothing about USB. Deciding staleness by watching for a
    // CHANGE instead does not work, and failed live: every setter here moves
    // m_mode/m_filter optimistically before the write goes out, so by the time
    // the radio's confirmation arrives the "did it move?" test compares the new
    // value against itself and says no. The symptom was every mode drawing AM's
    // 9 kHz window — a 9 kHz passband over a 3 kHz SSB filter — because the
    // connect-time read was never superseded.
    //
    // Recording the context the answer BELONGS TO instead is not fooled by an
    // optimistic write, because it is stamped only where the reply is decoded.
    CivMode m_ifWidthMode = CivMode::Usb;
    bool    m_ifWidthData = false;
    int     m_ifWidthSlot = 0;

    // Twin PBT, 0..255 with 128 centred. Together they slide the passband;
    // apart they narrow it from the inside. Defaulting to centre means a radio
    // that has not answered yet draws an unshifted window rather than a window
    // shoved to one end.
    int m_pbtInner = kPbtCentreCode;
    int m_pbtOuter = kPbtCentreCode;

    // TRANSMIT passband. m_txBandwidthSlot is what 16 58 reported — 0 WIDE,
    // 1 MID, 2 NAR, -1 not yet known — and decides WHICH stored slot a
    // setTxFilter() write reshapes. The Hz pair is the last one READ BACK from
    // the radio, so what the Phone applet shows is the passband the transmitter
    // has rather than the one that was asked for.
    int m_txBandwidthSlot = -1;
    int m_txFilterLowHz = 0;
    int m_txFilterHighHz = 0;

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
    std::optional<bool> m_repeaterToneOn;
    std::optional<double> m_repeaterToneHz;
    std::optional<icom::RepeaterOffsetDirection> m_repeaterOffsetDirection;
    std::optional<int> m_repeaterOffsetHz;
    std::optional<std::uint8_t> m_repeaterAccess;
    std::optional<double> m_repeaterRxToneHz;
    std::optional<int> m_repeaterDtcsCode;
    std::optional<bool> m_repeaterDtcsTxReverse;
    std::optional<bool> m_repeaterDtcsRxReverse;
    std::optional<std::uint64_t> m_repeaterTxFrequencyHz;
    int     m_controlPollPhase = 0;
    bool    m_rxAntennaExternal = false;
    std::optional<bool> m_radioDialLocked;

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
    // The carrier owns a 20 ms radio-rate producer while TUNE is active. It
    // cannot depend on microphone capture callbacks: PC Audio may be disabled,
    // and then a keyed IC-705 receives no samples at all. Exact 20 ms frames
    // match the RS-BA1 packetizer's framing without borrowing the mic stream.
    bool m_tuning = false;
    // Last non-off value reported by 16 47. The shared UI is still boolean,
    // so remembering 01 vs 02 is what lets OFF -> ON restore Full rather than
    // silently demoting it to Semi.
    int m_cwBreakInMode = 1;
    int m_preTuneTxPowerPercent = -1;
    double m_tunePhase = 0.0;
    static constexpr double kTuneToneHz = 1500.0;
    static constexpr int kTuneToneFrameMs = 20;
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
    void publishPhoneModulationLevel();

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
    quint64 m_schedulerCancelledRequests = 0;
    quint64 m_schedulerFailedRequests = 0;
    bool m_civBacklogIncidentReported = false;

    // Last structured incident survives a dropped session so support can read
    // it after the sockets are gone. It is replaced only by a newer incident
    // or a successfully connected new session.
    QVariantMap m_lastIncident;
    quint64 m_incidentSequence = 0;
    qint64 m_connectedAtMs = 0;

    // CI-V stall detection. The transport can be healthy while the command
    // plane is dead — see onLinkTick — so these track the command plane alone.
    qint64  m_lastInboundCivAtMs = 0;
    QString m_lastOutboundCiv;      // the last frame we sent, as hex
    QString m_lastOutboundCivKey;   // payload-free semantic transaction id
    qint64  m_lastOutboundCivAtMs = 0;
    bool    m_civStallReported = false;
    qint64  m_civRecoveryStartedAtMs = 0;
    qint64  m_lastCivRecoveryAttemptAtMs = 0;
    int     m_civRecoveryAttempts = 0;
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
