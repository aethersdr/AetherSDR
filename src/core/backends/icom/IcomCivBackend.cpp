#include "core/backends/icom/IcomCivBackend.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>

#include "core/backends/icom/IcomControls.h"
#include "core/backends/icom/IcomSettings.h"
#include "core/Resampler.h"

namespace AetherSDR::icom {
namespace {

// The pan intents are the two that most need to say what they DECIDED rather
// than what they were asked, because both of them deliberately do something
// other than the literal request: one refuses, the other quantises.
Q_LOGGING_CATEGORY(lcIcomPan, "aether.icom.pan")

// Link health. Separate from the pan category because the one thing anyone
// wants to switch on after a hang is the stall warning, and nothing else.
Q_LOGGING_CATEGORY(lcIcomLink, "aether.icom.link")
Q_LOGGING_CATEGORY(lcIcomScheduler, "aether.icom.scheduler")

// Which CI-V address we ended up talking to, and why. Its own category because
// a wrong address is SILENT — the radio simply never answers — so when the
// symptom is "connected but nothing works", this is the one trace that
// distinguishes a wrong address from a dead command plane, and nobody wants the
// meter traffic alongside it.
Q_LOGGING_CATEGORY(lcIcomAddr, "aether.icom.address")

// EVERY CI-V FRAME, both directions, as hex.
//
// The in-memory ring behind `civ trace` already recorded these, but it dies
// with the backend — disconnect and the evidence is gone, which is exactly
// when you want it. A log category survives the session and can be read after
// the fact.
//
// This is the difference between three indistinguishable failures: the query
// was never sent, the radio never answered, or the answer arrived and our
// decode rejected it. Diagnosing a mode-reporting bug without it means
// inferring from published state, which cannot tell those apart.
Q_LOGGING_CATEGORY(lcIcomCiv, "aether.icom.civ")

// Metering is examined this often; the MeterPoller decides what is actually
// due. This is ALSO the scheduler's pump, which is why it is 10 ms and not the
// 40 ms the meter intervals alone would justify: IcomCivScheduler releases at
// most one frame per kSlotMs (25 ms), and a tick slower than the slot would
// stretch the effective dispatch interval to tick + slot. At 10 ms the pump
// costs a `due()` scan and one takeNext() on an empty queue, and the slot —
// not this timer — remains what paces the wire.
constexpr int kMeterTickMs = 10;
// Transport counters publish on a FIXED cadence, not on receive: "nothing
// arrived this second" is the observation the heartbeat's alarm path waits for,
// and a backend that emits only on receive can never report its own silence.
constexpr int kLinkTickMs = 1000;
// How far the operator may drag before it counts as a tune, as a fraction of
// the scope's HALF-span (m_scopeSpanHz). See setPanCenter for why a dead
// zone is needed at all: a click with a pixel of hand movement arrives as a
// centre request, and without this every stray click moved the dial.
constexpr double kPanDragDeadZoneFraction = 0.01;

QByteArray floatBytes(const std::vector<float>& v)
{
    return {reinterpret_cast<const char*>(v.data()),
            static_cast<qsizetype>(v.size() * sizeof(float))};
}

}  // namespace

IcomCivBackend::IcomCivBackend(QObject* parent)
    : IRadioBackend(parent), m_model(&unknownModel())
{
    // MONOTONIC, NOT WALL CLOCK. Every timestamp in this file measures an
    // INTERVAL — a dispatch slot, a reply timeout, a poll period, a stall
    // threshold, a frame's age in the trace — and none is ever reported as an
    // absolute time. Wall clock was therefore never the right source, and once
    // every CI-V producer runs through one scheduler it is an actively
    // dangerous one: a backward step (an NTP correction after suspend/resume
    // being the realistic case) makes every `now - then` negative at once, so
    // the dispatch slot never opens, the in-flight read never times out, and
    // the stall detector never warns. That is a silent, total command-plane
    // freeze — meters, controls, PTT poll and operator writes alike —
    // recoverable only by reconnecting. QElapsedTimer cannot step backwards.
    m_clock.start();
}

qint64 IcomCivBackend::nowMs() const
{
    return m_clock.elapsed();
}

IcomCivBackend::~IcomCivBackend() = default;

// ---------------------------------------------------------------------------
// Capability
// ---------------------------------------------------------------------------

RadioCapabilities IcomCivBackend::capabilities() const
{
    const IcomModel& m = *m_model;
    RadioCapabilities c;
    c.family = QStringLiteral("icom");
    c.manufacturer = QStringLiteral("Icom");
    c.model  = m_deviceName.isEmpty() ? QString::fromUtf8(m.name.data(),
                                                          static_cast<int>(m.name.size()))
                                      : m_deviceName;

    c.maxSlices = m.receivers;
    c.maxPanadapters = m.hasScope ? m.receivers : 0;
    c.tuningMinHz = static_cast<double>(m.tuningMinHz);
    c.tuningMaxHz = static_cast<double>(m.tuningMaxHz);

    c.canTransmit = m.hasTransmit;
    c.txPowerMaxWatts = m.txPowerMaxWatts;

    // The scope scale is OURS, not the radio's: it comes from ScopeCalibration
    // (floor/span, shifted by the radio's own reference level), and there is no
    // CI-V command to set a display dBm range — this backend has no consumer for
    // one. Leaving this true made the noise-floor auto-adjust chase an echo that
    // can never arrive; see RadioCapabilities::radioOwnsDbmScale.
    c.radioOwnsDbmScale = false;

    // The RADIO modulates. Contrast the HL2, where the host does — this drives
    // the mic-source list and the PC-audio lock, so getting it wrong opens the
    // host microphone on a radio that will never use it.
    c.hostModulates = false;

    // ...but the host still SHIPS the audio. The radio modulates from PCM we
    // send over its own UDP stream, so the transmit capture and DSP chain must
    // run here even though no modulator does.
    c.takesTxAudioOverSeam = true;

    // NR / NB / notch are 0x16 commands executed in the radio's own firmware.
    c.hasRadioSideDsp = true;

    // ...but NOT FlexRadio's particular set of it. NRL, ANFL and ANFT are WDSP
    // LMS/FFT filters with no register anywhere on this radio, so before this
    // flag existed hasRadioSideDsp lit up three buttons that reached nothing —
    // the operator toggles them, the setting persists, the audio is unchanged.
    c.hasLmsNoiseFilters = false;

    // The radio's own single in-passband notch: 16 48 enables it, 14 0D places
    // it, 16 57 picks one of three widths. Not a TNF and not the auto notch —
    // see the capability's own note.
    c.hasManualNotch = true;

    // The radio's own blanker, reached over CI-V and already covered by
    // hasRadioSideDsp. A networked Icom ships finished audio, not IQ (see
    // hasDaxStreams below), so there is nothing here for a host stage to blank
    // even if we wanted one.
    c.hasHostNoiseBlanker = false;

    // NO IQ, on any networked Icom. Not deferred — absent. See icom-oracle §8.1.
    c.hasDaxStreams = false;

    // The radio HAS a GPS and the protocol will not carry its data.
    c.hasGpsLocation = false;

    c.hasSupplyVoltageTelemetry = true;   // 0x15 0x15 Vd

    // THE ATU BUTTON IS REACHABLE AGAIN.
    //
    // `1C 01` drives an EXTERNAL AH-705 and there is no command to ask whether
    // one is attached, so this capability is genuinely unanswerable from the
    // radio. It was false on the reasoning that a button which might do nothing
    // is worse than no button — but that reasoning cost every IC-705 operator
    // who DOES own an AH-705 the only way to reach it, and the radio reports
    // its tuner state (1C 01 read) well enough for the button to tell the truth
    // once a cycle has run.
    //
    // So: offered, and honest about the outcome rather than about the hardware.
    // A start on a radio with no tuner reports NONE and the button returns to
    // rest, which is a better answer than a control that is not there.
    c.hasTuner = m.hasTransmit;

    // The radio chooses its own modulation input from its own menu (MOD Input
    // > DATA MOD, which must be WLAN for us to be heard at all). A client
    // cannot pick MIC / BAL / LINE / ACC, so the Phone applet collapses to PC.
    c.hasSelectableMicInputs = false;

    // THREE, and only three — and WHICH three depends on the mode. FIL1 is
    // 3.0 kHz in SSB, 1.2 kHz in CW, 9 kHz in AM and 15 kHz in FM, so a single
    // fixed list is wrong in every mode but one. This is republished on every
    // mode change (see setSliceMode / the mode decode), which is what stops the
    // filter buttons offering widths that all land on the same slot.
    //
    // The values are the radio's own defaults, which the operator can redefine
    // in its SET menu and we cannot read back — so these are the best available
    // labels, not a promise about the passband.
    if (m_model->hasScope || m_model->isKnown())
        {
        // std::vector<int> from the codec (which stays Qt-free) into the
        // QList the capability struct carries.
        const auto widths = filterWidthsForMode(currentLadderMode().toStdString());
        c.rxFilterWidthsHz = QList<int>(widths.begin(), widths.end());
    }

    c.hasProfiles = false;
    c.hasWaveforms = false;
    c.hasMultiClientSessions = false;
    c.hasRadioSideWaterfallAutoBlack = false;
    c.persistsMemories = false;

    // A one-way trip over WiFi: 0x18 0x00 powers the radio off, which drops the
    // WLAN interface, so the 0x18 0x01 that would bring it back has no path.
    c.canReboot = false;

    // EMPTY, and load-bearing. An Icom remembers its own frequency, mode and
    // filter across power cycles and reports them on request, so Constitution
    // II/III says the client must not re-assert them. This backend READS state
    // at connect; it never pushes a restored one.
    c.clientSettingsDomains = {};
    c.extensionNamespaces << QStringLiteral("icom");

    return c;
}

void IcomCivBackend::publishCapabilities() { emit capabilitiesChanged(); }

void IcomCivBackend::publishScopeDbmRange()
{
    // kUnknown has hasScope=false, so this is a quiet no-op on a backend whose
    // radio has not identified itself yet — which is correct: there is no scope
    // to draw an axis for, and the connect path publishes once the model is
    // known. (m_model is never null; the constructor seeds it with
    // unknownModel().)
    if (!m_model->hasScope)
        return;

    // THE AXIS MUST MATCH THE DECODER, INCLUDING THE SIGN.
    //
    // toDbm() maps a sample to `floorDbm + (v/max)*spanDb - referenceDb`, so
    // raising the radio's reference level moves the decoded trace DOWN in dBm.
    // The axis has to move the same way. An earlier version of this added
    // referenceDb here while toDbm subtracted it, which left the scale wrong by
    // 2x the reference whenever it was non-zero — invisible at the default 0,
    // and a growing error the further the operator moved it.
    //
    // Derived from the same ScopeCalibration toDbm() uses rather than repeating
    // the arithmetic, so the two cannot drift apart again.
    const double floorDbm = m_scopeCal.floorDbm - m_scopeCal.referenceDb;
    emit panRangeChanged(panId(), floorDbm, floorDbm + m_scopeCal.spanDb);
}

QString IcomCivBackend::currentNeutralMode() const
{
    return QString::fromStdString(modeToNeutral(m_mode, m_dataMode));
}

// THE MODE THE FILTER LADDER IS KEYED ON, which is not always the neutral one.
//
// AetherSDR has no RTTY neutral mode, so modeToNeutral collapses RTTY/RTTY-R to
// DIGL/DIGU — correct for the slice's mode indicator and wrong for the filter
// ladder, because an IC-705 in RTTY runs 2.4k/500/250 where SSB runs
// 3.0k/2.4k/1.8k. Feeding the collapsed name to CivCodec's ladder made its RTTY
// row unreachable and published the SSB widths on a radio in RTTY: the button
// labelled "1.8k" selected FIL3, which is 250 Hz there, and the passband drawn
// over the waterfall was seven times the one actually in circuit. The operator
// can only get here from the radio's own front panel, which is exactly the case
// this backend's connect-time adoption exists to respect.
QString IcomCivBackend::currentLadderMode() const
{
    if (m_mode == CivMode::Rtty)
        return QStringLiteral("RTTY");
    if (m_mode == CivMode::RttyR)
        return QStringLiteral("RTTYR");
    return currentNeutralMode();
}

void IcomCivBackend::publishModeState()
{
    const QString neutral = currentNeutralMode();
    if (neutral.isEmpty())
        return;   // D-STAR: a waveform, not a demodulator setting
    SliceDelta s;
    s.mode = neutral;
    // The passband travels WITH the mode, in the same delta, because the radio
    // will never send one. Applied after the mode by SliceModel's own ordering,
    // which is what stops a narrow CW window surviving into DIGU.
    const auto [low, high] =
        passbandForModeAndFilter(currentLadderMode().toStdString(), m_filter);
    s.filterLow  = low;
    s.filterHigh = high;
    emit sliceChanged(sliceId(), s);
    // The filter LADDER changes with the mode, so the buttons have to be
    // rebuilt from the new one. Change-gated inside the models, so the repeat
    // this produces on an unchanged mode costs nothing.
    publishCapabilities();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void IcomCivBackend::connectRadio(const RadioConnectRequest& request)
{
    disconnectRadio();

    IcomSession::Params p;
    p.host = QHostAddress(request.host);
    p.controlPort = request.port ? request.port : kControlPort;
    p.serialPort  = static_cast<quint16>(
        request.params.value(QStringLiteral("icom.serialPort"), kSerialPort).toUInt());
    p.audioPort   = static_cast<quint16>(
        request.params.value(QStringLiteral("icom.audioPort"), kAudioPort).toUInt());
    p.username = request.params.value(QStringLiteral("icom.username")).toString();
    p.password = request.params.value(QStringLiteral("icom.password")).toString();
    // "AUTO" IS CARRIED, NOT COLLAPSED.
    //
    // This line used to read the address with 0xA4 as its default, which made an
    // absent parameter and a deliberate IC-705 pick the same input. They are not
    // the same: 0xA4 is right for one model in kModels and silently ignored by
    // every other, and CI-V has no error for "nobody is at that address" — the
    // radio simply never answers, so the session comes up, publishes the
    // conservative unknown capabilities, and reads as a half-finished backend.
    //
    // An absent parameter now means AUTO: seed the address from whatever the
    // RS-BA1 handshake names the radio, then let its own 0x19 0x00 reply correct
    // that. 0xA4 survives only as the last fallback, for a radio that neither
    // names itself recognisably nor answers the broadcast.
    const bool haveCiv = request.params.contains(QStringLiteral("icom.civAddress"));
    const uint civParam = request.params.value(QStringLiteral("icom.civAddress"), 0).toUInt();
    const bool civValid = haveCiv && civParam > 0 && civParam <= 0xFF;
    p.civAddress = civValid ? static_cast<std::uint8_t>(civParam)
                            : IcomSettings::kDefaultCivAddress;
    // Only a TYPED address pins the destination. See IcomSettings::CivSelection.
    m_civAddressPinned =
        civValid && request.params.value(QStringLiteral("icom.civAddressPinned")).toBool();
    m_civSeedAddress = p.civAddress;
    m_civReported = 0;
    m_civAmbiguous = false;
    m_connectBurstSent = false;
    m_modelByName = nullptr;
    // 48 kHz, FIXED — the rate is deliberately not negotiable here.
    //
    // It is tempting on a weak link: 48 kHz LPCM is ~768 kbps each way, and a
    // 2.4 GHz path with power-save latency genuinely struggles with it. But the
    // rate cannot move on its own. The 1364/556 packet split is sized for a
    // 20 ms frame AT this rate, and lowering the rate without re-deriving the
    // split produces frames of the wrong DURATION — measured at 16 kHz: 60 ms
    // frames, discarded by the radio's jitter buffer, a keyed transmitter with
    // zero forward power and nothing on the air or on the radio's own scope.
    //
    // The codecs that would reduce bandwidth without touching framing are not
    // available either: wfview force-downgrades Opus and ADPCM to LPCM16 unless
    // the peer is another wfview SERVER, so on real Icom hardware they do not
    // exist. kappanhang, which is byte-exact for this radio, only ever speaks
    // 48 kHz LPCM 1ch 16-bit.
    //
    // So this mirrors kappanhang, and the connect path deliberately offers no
    // way to change it.
    m_audioRateHz = kRadioAudioRateHz;
    p.sampleRateHz = static_cast<quint32>(m_audioRateHz);

    m_session = std::make_unique<IcomSession>();
    connect(m_session.get(), &IcomSession::connected, this, &IcomCivBackend::onSessionConnected);
    connect(m_session.get(), &IcomSession::disconnected, this,
            &IcomCivBackend::onSessionDisconnected);
    connect(m_session.get(), &IcomSession::civFrameReady, this, &IcomCivBackend::onCivFrame);
    connect(m_session.get(), &IcomSession::audioReady, this, &IcomCivBackend::onAudio);

    if (!m_session->start(p))
        emit connectionError(QStringLiteral("could not open the Icom session"));
}

void IcomCivBackend::disconnectRadio()
{
    // Teardown is not allowed to wait for an ordinary outstanding read.  Drop
    // all background work, fail-safe unkey on every connected disconnect, and
    // restore the operator's RF-power setpoint if TUNE had borrowed it.  These
    // are response-free emergency dispatches so both datagrams reach the
    // session before its sockets close; no state is optimistically adopted.
    if (m_session && m_connected) {
        m_civScheduler.reset();
        queueEmergencyWriteNoReply(cmdSetPtt(m_session->civAddress(), false), "ptt");
        if (m_tuning && m_preTuneTxPowerPercent >= 0) {
            queueEmergencyWriteNoReply(
                cmdSetLevel(m_session->civAddress(), level::kRfPower,
                            percentToLevelRaw(m_preTuneTxPowerPercent)),
                "level.rfPower");
        }
        const qint64 now = nowMs();
        pumpCiv(now);
        pumpCiv(now);
    }

    for (QTimer** t : {&m_meterTimer, &m_linkTimer, &m_civDetectTimer}) {
        if (*t) {
            (*t)->stop();
            (*t)->deleteLater();
            *t = nullptr;
        }
    }
    if (m_session) {
        m_session->stop();
        m_session.reset();
    }
    m_rxResampler.reset();
    m_scope.reset();
    m_meters.reset();
    m_civScheduler.reset();
    m_schedulerTimeoutsReported = 0;
    serviceSchedulerWaiters(nowMs());
    m_pendingPttIntent.reset();
    m_pendingPttUntilMs = 0;
    // The radio keeps its own DSP state across our sessions and we have not
    // read it back, so "unknown" is the only honest starting point — carrying
    // the last session's belief would suppress the first command that matters.
    m_nrEnableSent = m_nbEnableSent = m_anfEnableSent = m_mnEnableSent = -1;
    // Same reasoning, applied to every OTHER control: the scrub mirrors are
    // stale the moment the session ends, so a scrub run after a reconnect that
    // dropped a read must report NOT-TESTED rather than re-asserting the
    // previous session's belief. The two observation sets are cleared with it
    // so `controls map`'s seenThisSession/sentThisSession columns mean what
    // they say across a reconnect.
    m_controlsValueKnown.clear();
    m_controlsSeen.clear();
    m_controlsSent.clear();
    m_controlsScheduled.clear();
    m_framesObserved = 0;
    // The CI-V address resolution is per-session for the same reason: the
    // operator can move to a different radio, or change the address on this one,
    // between sessions. An auto-detected value is DETECTED, never CHOSEN — it is
    // never written back to settings, so "auto" survives more than one session.
    m_civReported = 0;
    m_civAmbiguous = false;
    m_connectBurstSent = false;
    m_modelByName = nullptr;
    m_scopeStarted = false;
    m_tuning = false;
    m_preTuneTxPowerPercent = -1;
    if (m_connected) {
        m_connected = false;
        emit disconnected();
    }
}

bool IcomCivBackend::isConnected() const { return m_connected; }

// The connect-edge read burst.
//
// Kept as one named snapshot so every connect and address retarget enters the
// same scheduler path:
//
//   * a radio whose NAME we do not recognise has to learn its CI-V address from
//     the broadcast reply before there is a correct address to burst at, and
//   * a retarget has to RE-ISSUE it. Those reads went to an address nobody was
//     answering on, so they returned nothing; re-sending them at the address the
//     radio actually reported is the only thing that recovers the session, and
//     it is cheap because it happens at most once per connect.
//
// The order below expresses startup preference only. IcomCivScheduler paces the
// frames, coalesces duplicates, and keeps this snapshot from becoming the
// connect-edge burst called out by RFC #4983.
void IcomCivBackend::sendConnectReadBurst()
{
    if (!m_session)
        return;
    // Re-entrancy: a retarget arriving mid-burst would otherwise stack.
    m_connectBurstSent = true;
    if (m_civDetectTimer)
        m_civDetectTimer->stop();

    // ASK the radio what it is. The CI-V address is user-changeable and several
    // models speak this same transport, so a hardcoded 0xA4 would silently
    // mis-decode an IC-9700 someone pointed this at.
    //
    // Still DIRECTED, alongside the broadcast the caller sent: this one confirms
    // that the address we are actually using has something behind it, which the
    // broadcast cannot tell us on a bus with more than one device.
    const auto queueStartupRead = [this](const std::vector<std::uint8_t>& frame) {
        queueRead(frame, semanticKey(frame), IcomCivScheduler::Priority::Maintenance);
    };
    queueRead(cmdReadId(m_session->civAddress()), "identity.directed",
              IcomCivScheduler::Priority::Maintenance);
    queueStartupRead(cmdReadFrequency(m_session->civAddress()));
    queueStartupRead(cmdReadMode(m_session->civAddress()));
    // ...AND WHETHER THAT MODE IS A DATA MODE. 04 answers USB for both USB and
    // USB-D, so without this a radio the operator left in USB-D was adopted as
    // plain USB at every connect — the mode indicator, the passband and the
    // modulation-source diagnostic all decided for the wrong mode, and the
    // first thing AetherSDR wrote pushed the radio the rest of the way out of
    // DATA. 26 00 reports mode, DATA and filter together, so it also corrects
    // the 04 above if the two ever disagree.
    if (m_model->hasVfoModeCommand)
        queueStartupRead(cmdReadVfoMode(m_session->civAddress()));

    // ASK WHERE THE RADIO TAKES ITS MODULATION FROM, using this model's own
    // guide. The SET-menu numbers and enum values differ even between the
    // IC-705 and IC-7300MK2, so an unknown model is deliberately left unread.
    if (const auto mod = modulationProfileFor(*m_model)) {
        for (int item : {mod->dataOffInputItem, mod->dataInputItem,
                         mod->usbLevelItem, mod->accessoryLevelItem,
                         mod->networkLevelItem}) {
            if (item >= 0) {
                queueStartupRead(cmdReadSetting(m_session->civAddress(), item));
            }
        }
    }

    // ADOPT THE RADIO'S OWN LEVELS. Constitution II/III says an Icom is
    // authoritative over its operating state and the client must never push a
    // restored one — but that cuts both ways, and the reading half was missing.
    // Every control opened at its construction default instead: the power
    // slider said one thing while the radio ran at another, and the first touch
    // of any control JUMPED the radio to the UI's invented value rather than
    // nudging it from where it actually was.
    //
    // Read-only. Nothing here writes; each answer is decoded in onCivFrame and
    // published as a delta, exactly as an unsolicited change would be.
    for (std::uint8_t which : {level::kRfPower, level::kAf, level::kSquelch,
                               level::kMicGain, level::kCompLevel, level::kMonitor,
                               level::kNrLevel, level::kNbLevel,
                               level::kNotchPos, level::kRf, level::kVoxGain})
        queueStartupRead(cmdReadLevel(m_session->civAddress(), which));

    // ...and the switches, which have the same problem: the applet toggles all
    // read "off" on a radio that may have NR or the compressor running.
    for (std::uint8_t fn : {func::kPreamp, func::kAgc, func::kNoiseReduce,
                            func::kNoiseBlanker, func::kAutoNotch,
                            func::kManualNotch,
                            func::kCompressor, func::kMonitorFn, func::kVox})
        queueStartupRead(cmdReadFunction(m_session->civAddress(), fn));

    // The attenuator is NOT sub-addressed, so it needs its own read rather than
    // a slot in the loop above.
    queueStartupRead(cmdReadAttenuator(m_session->civAddress()));
    // RIT / XIT and the antenna tuner. All four were write-only: the controls
    // opened at OUR defaults, so an operator who set RIT on the radio and
    // reconnected saw zero on a rig that was still offset.
    for (std::uint8_t sub : {tuneOffset::kFrequency, tuneOffset::kRitOnOff,
                             tuneOffset::kXitOnOff})
        queueStartupRead(cmdReadTuneOffset(m_session->civAddress(), sub));
    queueStartupRead(cmdReadTuner(m_session->civAddress()));
}

// The radio answered 0x19 0x00. Decide whether to believe it, and where that
// leaves the destination.
//
// IDENTITY always comes from here — modelForCivAddress() is one of exactly two
// writers of m_model, and both read the wire rather than the operator's pick, so
// capabilities have always followed the actual radio. What is new is that the
// DESTINATION can follow it too.
void IcomCivBackend::adoptReportedCivAddress(std::uint8_t reported)
{
    if (reported == 0)
        return;

    // ONCE AMBIGUOUS, ALWAYS AMBIGUOUS for this session — and this guard is
    // load-bearing rather than defensive.
    //
    // The revert below re-issues the read burst at the seed address, but the
    // burst sent at the *adopted* address is already in flight and carries its
    // own directed 0x19 0x00. That reply lands after the revert, matches
    // m_civReported exactly (so it is not a third address), falls through to the
    // retarget branch, and quietly puts the session back on the responder we had
    // just decided we could not trust — undoing the revert with no warning.
    //
    // Found by tracing the two-responder test rather than by running it: the
    // test asserted the identity and the warning, both of which survive the
    // regression, and passed either way.
    if (m_civAmbiguous)
        return;

    // TWO DIFFERENT RESPONDERS — adopt NEITHER, and go back to the seed.
    //
    // Broadcast on a point-to-point LAN radio is unambiguous, and both lab
    // radios behaved that way. But Icom's own RS-BA1 server can front a real
    // serial CI-V bus carrying a second radio, a rotator or an amplifier, and
    // every one of those answers 0x00. Picking whichever replied first would
    // decode the rest of the session against a device the operator never chose
    // — silently, and with a plausible-looking result.
    //
    // Unmeasured here: both lab radios are single direct-LAN devices. The
    // handling is deliberately the conservative one for a case we have reasoned
    // about but not reproduced.
    if (m_civReported != 0 && m_civReported != reported) {
        if (!m_civAmbiguous) {
            m_civAmbiguous = true;
            qCWarning(lcIcomAddr) << "two CI-V addresses answered the broadcast:"
                              << Qt::hex << m_civReported << "and" << reported
                              << "- adopting neither";
            emit configurationWarning(
                QStringLiteral("More than one device answered on this CI-V bus "
                               "(%1 and %2). Choose the radio's model, or enter its "
                               "CI-V address, so AetherSDR knows which one to use.")
                    .arg(QString::number(m_civReported, 16).toUpper(),
                         QString::number(reported, 16).toUpper()));
            // GIVE BACK THE IDENTITY FIRST, which the first responder had
            // already been allowed to set. Reverting the address alone leaves
            // the far worse half of the problem in place: capabilities — TX
            // power ceiling, band ranges, scope geometry — would go on
            // describing a device we have just decided we cannot identify,
            // while the frames go somewhere else entirely. Caught by the
            // two-responder test, which passed on the address assertion alone.
            //
            // BEFORE the revert below, because the burst that revert re-issues
            // is shaped by m_model: reverting the address first would send it
            // built against the very identity we are withdrawing.
            m_model = m_modelByName ? m_modelByName : &unknownModel();

            // Back to what the operator or the handshake gave us. That is a
            // choice with a reason behind it; "whoever spoke first" is not.
            // Discard everything queued for the responder we no longer trust,
            // including a directed identity read that may already be in flight.
            // The replacement snapshot below is the only work allowed to
            // survive the destination change.
            m_civScheduler.reset();
            if (m_session) {
                if (m_session->civAddress() != m_civSeedAddress)
                    m_session->setCivAddress(m_civSeedAddress);
                sendConnectReadBurst();
                // Same destination argument as the retarget path below: the
                // switches went to a responder we have just walked away from.
                m_scopeStarted = false;
                applyScopeStartup();
            }
            publishCapabilities();
            RadioDelta r;
            r.model = QString::fromUtf8(m_model->name.data(),
                                        static_cast<int>(m_model->name.size()));
            emit radioChanged(r);
        }
        return;
    }
    m_civReported = reported;

    if (!m_session)
        return;

    // A TYPED address is a device selection and outranks the wire on
    // destination — but the disagreement is still worth naming, because on a
    // point-to-point radio it means the typed address is simply wrong and the
    // symptom (a connected radio that answers nothing) names no cause at all.
    if (m_civAddressPinned) {
        if (reported != m_session->civAddress()) {
            qCWarning(lcIcomAddr) << "radio reports CI-V address" << Qt::hex << reported
                              << "but the entered address" << m_session->civAddress()
                              << "was kept - it selects the device";
            emit configurationWarning(
                QStringLiteral("This radio reports CI-V address %1, not the %2 that "
                               "was entered. The entered address is being used.")
                    .arg(QString::number(reported, 16).toUpper(),
                         QString::number(m_session->civAddress(), 16).toUpper()));
        }
        return;
    }

    if (reported == m_session->civAddress()) {
        qCInfo(lcIcomAddr) << "CI-V address confirmed by the radio:" << Qt::hex << reported;
        return;
    }

    // RETARGET. The seed was a model-table lookup or an operator's model pick,
    // and the radio has just said otherwise about itself — which it is entitled
    // to do, because the address is changeable on the radio's own front panel
    // and nothing on our side can know that.
    // No queued or in-flight command addressed to the old destination can be
    // reused after this point. In particular, semantic read coalescing must not
    // mistake an old-address startup read for the replacement snapshot.
    m_civScheduler.reset();
    qCInfo(lcIcomAddr) << "retargeting CI-V from" << Qt::hex << m_session->civAddress()
                   << "to the address the radio reported:" << reported;
    m_session->setCivAddress(reported);

    // RESOLVE THE MODEL BEFORE THE BURST, not after it.
    //
    // onCivFrame does this same lookup a few lines further on, together with the
    // publishing that belongs with it - but it runs AFTER this, and the burst
    // below is not model-neutral: it gates the 0x26 DATA-mode read on
    // hasVfoModeCommand, which only a resolved model sets. Re-issuing first and
    // resolving second therefore drops that read on precisely the path that
    // needs it most - an unrecognised handshake name in front of a radio the
    // table does know, which is the RS-BA1-server shape this feature exists to
    // survive. Without the read, a radio left in USB-D is adopted as plain USB
    // and our first write pushes it the rest of the way out of DATA (#4984).
    //
    // Cheap and idempotent: the lookup below repeats it and does the publishing.
    if (const IcomModel* byAddress = modelForCivAddress(reported))
        m_model = byAddress;

    // The burst either has not run yet (unknown model, waiting on exactly this
    // reply) or ran against an address nobody answered on. Both want it sent at
    // the address that just answered, so there is no branch here.
    sendConnectReadBurst();

    // AND THE SCOPE SWITCHES, for the same reason the burst goes again.
    //
    // "Started" was per SESSION, but these are addressed frames, so what it has
    // to mean is per DESTINATION. A radio whose name resolved had its scope
    // switched on at the connect edge - addressed to the seed, where by this
    // function's own premise nobody is listening - and the latch then refused to
    // send them anywhere else for the rest of the session. Everything else
    // recovers here, so the session reads as healthy while capabilities() goes
    // on advertising a panadapter that can never fill: the exact black-panadapter
    // symptom applyScopeStartup() exists to prevent.
    m_scopeStarted = false;
    applyScopeStartup();

    // SAY WHAT HAPPENED — but only when it is something the operator could not
    // work out from the radio name and a working panadapter.
    //
    // "Auto" that never explains itself is undebuggable, and it is also how this
    // feature gets judged. But a message on every ordinary resolution is noise:
    // when the reported address maps straight back to a model in the table, the
    // operator already sees that model's name and its scope, and there is
    // nothing to report.
    const IcomModel* byWire = modelForCivAddress(reported);
    const QString addrHex = QString::number(reported, 16).toUpper();
    if (byWire && m_modelByName && byWire != m_modelByName) {
        // The radio's NAME and its ADDRESS name different models. Rare, and
        // genuinely ambiguous — an RS-BA1 server fronting one radio while
        // reporting its own name would look exactly like this.
        emit configurationWarning(
            QStringLiteral("This radio reports its name as %1 but its CI-V address "
                           "(%2) belongs to an %3. Using the address.")
                .arg(QString::fromUtf8(m_modelByName->name.data(),
                                       static_cast<int>(m_modelByName->name.size())),
                     addrHex,
                     QString::fromUtf8(byWire->name.data(),
                                       static_cast<int>(byWire->name.size()))));
    } else if (!byWire && m_modelByName) {
        // The address was changed on the radio's own front panel. Nothing is
        // wrong and nothing needs doing — this is the case auto-detect exists
        // for, and it is worth one line so "why does it say 50?" has an answer.
        emit configurationWarning(
            QStringLiteral("%1 is using CI-V address %2 — detected automatically.")
                .arg(QString::fromUtf8(m_modelByName->name.data(),
                                       static_cast<int>(m_modelByName->name.size())),
                     addrHex));
    } else if (!byWire) {
        // Neither signal resolved a model, so unknownModel()'s conservative
        // capabilities stand: no scope, no transmit. EXPLAINING that is the
        // point — today the operator gets the same reduced radio with no clue
        // why, and reads it as a half-finished backend rather than a model we
        // have no numbers for.
        emit configurationWarning(
            QStringLiteral("Connected on CI-V address %1, which is not a model "
                           "AetherSDR has data for — scope and transmit stay off. "
                           "Frequency and mode still work.")
                .arg(addrHex));
    }
}

void IcomCivBackend::onSessionConnected(const QString& deviceName)
{
    m_deviceName = deviceName;
    m_connected = true;

    // RESOLVE THE MODEL FROM THE NAME, NOW.
    //
    // capabilities() answers from m_model, which starts as unknownModel() —
    // deliberately conservative: no scope, NO TRANSMIT. That default is right
    // for a radio we cannot characterise, and wrong the moment we can: the
    // 0x19 0x00 address query needs a serial stream that does not exist until
    // after this point, so anything reading capabilities on the connect edge
    // saw canTransmit=false and refused to key a radio that transmits fine.
    // radiocert's meters and tx phases both did exactly that.
    //
    // The capabilities packet already told us the name during the handshake, so
    // use it. The address query still runs and still wins — it is the
    // authority, this is just early enough to be useful.
    m_modelByName = modelForName(deviceName.toStdString());
    if (m_modelByName)
        m_model = m_modelByName;

    // WRONG DEVICE, said as early as it can be said.
    //
    // Keyed on the NAME, not the address, and that distinction is the whole
    // check. An address difference is the ordinary case — an operator who picked
    // "IC-9700" and later changed the address on the radio gets corrected below
    // and should hear nothing about it. A NAME difference is the operator having
    // reached a different radio than the one they selected, usually by typing
    // the bench rig's IP, and it is worth saying out loud: capabilities follow
    // the wire, so a 10 W radio quietly replaces the 100 W one they chose.
    //
    // A warning rather than a refusal. capabilities() answers from m_model,
    // which only modelForName() and modelForCivAddress() ever set — the wire,
    // never the pick — so TX ceilings, band ranges and scope geometry already
    // track the actual radio. That makes this a LABELLING problem, and blocking
    // the connect would ask the operator to fix by hand what we have already
    // fixed ourselves.
    if (m_civAddressPinned && m_modelByName) {
        if (const IcomModel* picked = modelForCivAddress(m_civSeedAddress);
            picked && picked != m_modelByName) {
            emit configurationWarning(
                QStringLiteral("Connected to %1, not the %2 this CI-V address "
                               "selects. Check the radio's IP address.")
                    .arg(QString::fromUtf8(m_modelByName->name.data(),
                                           static_cast<int>(m_modelByName->name.size())),
                         QString::fromUtf8(picked->name.data(),
                                           static_cast<int>(picked->name.size()))));
        }
    }

    // The radio's audio is 48 kHz mono; the seam's per-slice contract is 24 kHz
    // interleaved stereo. Built once here rather than per-buffer: r8brain is
    // stateful, and a fresh instance per callback restarts its filter history
    // every block, which is audible as a periodic tick.
    m_rxResampler = std::make_unique<Resampler>(
        static_cast<double>(m_audioRateHz), static_cast<double>(kEngineAudioRateHz), 4096);

    // SEED THE DESTINATION FROM THE NAME, before anything is addressed.
    //
    // This is the one wire the whole feature hangs on. The name resolved above
    // already IS a CI-V address — kModels holds both — and it is the only
    // identity that exists at this instant, because the 0x19 0x00 query needs a
    // serial stream that only just opened. Seeding here costs nothing and closes
    // the silent-dead-session gap on its own, for every model in the table.
    //
    // Skipped when the operator TYPED an address: that is a device selection on
    // a possibly-shared bus, and our table lookup does not get to overrule it.
    if (!m_civAddressPinned && m_modelByName) {
        m_session->setCivAddress(m_modelByName->civAddress);
        // ...AND THIS IS NOW THE SEED. m_civSeedAddress is where the two-
        // responder path reverts to when it decides no reported address can be
        // trusted, and its comment there promises "what the operator or the
        // handshake gave us" - but it was captured in connectRadio() from the
        // request param and never updated here, so the handshake half was not
        // true. An ambiguous bus therefore threw away a name-resolved address
        // that was very likely correct and fell back to the 0xA4 default.
        m_civSeedAddress = m_modelByName->civAddress;
    }

    // ONE BROADCAST 0x19 0x00 — the actual auto-detect, and the only frame this
    // change adds to the connect edge.
    //
    // CI-V is addressed, so a directed query can only ever confirm an address we
    // already believe; asked at 0x00 the radio answers with the address it
    // actually uses, whatever that is. Measured 2026-08-14 on both lab radios:
    // an IC-9700 replied fe fe e0 a2 19 00 a2 fd and an IC-705 fe fe e0 a4 19 00
    // a4 fd, while the same session's query to a bogus 0x12 drew only the bus
    // echo — so the answer discriminates rather than the radio replying to
    // everything. It needs no model table, so it resolves a radio kModels has
    // never heard of, and it is correct when the address was changed ON the
    // radio, which no table can be.
    //
    // SENT ONCE PER CONNECT. Never polled, never retried on a timer — see the
    // bounded wait below and RFC #4983.
    if (!m_civAddressPinned) {
        queueRead(cmdReadId(kBroadcastAddress), "identity.broadcast",
                  IcomCivScheduler::Priority::Maintenance);
    }

    // THE COMMON PATH STARTS WITHOUT AN AUTO-DETECT WAIT.
    //
    // Whenever the name resolved — both lab radios and all seven models in
    // kModels — the address is already right, so the snapshot is admitted now
    // and the broadcast above leads it through the shared scheduler as a
    // correction path. Only a radio whose name we do not recognise waits, and
    // only for as long as it takes to learn where to send the snapshot; sending
    // it to a guessed address first would be twenty frames to nobody.
    if (m_civAddressPinned || m_modelByName) {
        sendConnectReadBurst();
    } else {
        m_civDetectTimer = new QTimer(this);
        m_civDetectTimer->setSingleShot(true);
        connect(m_civDetectTimer, &QTimer::timeout, this, [this] {
            if (m_connectBurstSent)
                return;
            // NO REPLY. Burst at the fallback address anyway rather than leaving
            // the operator with a connected radio and no state at all — a silent
            // radio still deserves whatever a wrong-address session can give,
            // and this is exactly today's behaviour, reached only now.
            qCInfo(lcIcomAddr) << "no CI-V id reply within" << kCivDetectTimeoutMs
                           << "ms; falling back to address"
                           << Qt::hex << m_session->civAddress();
            sendConnectReadBurst();
        });
        m_civDetectTimer->start(kCivDetectTimeoutMs);
    }
    applyScopeStartup();

    // CONNECTED FIRST, then the state.
    //
    // RadioModel stages the previous session's slices and CLEARS m_slices on
    // the connect edge (stagePreviousSessionModelsForReconnect). Publishing the
    // slice before connected() therefore created it and had it swept away in
    // the same breath — the model ended with no slice at all, which is why
    // click-to-tune reported "Slice capacity is full" (the spectrum could not
    // resolve a tune target, so it fell through to the create-a-slice path
    // against a one-slice radio) and why txSlice never took.
    emit connected();
    publishCapabilities();

    // THE PAN FIRST, then the slice that names it.
    //
    // RadioModel maps a backend pan id to a neutral index on FIRST SIGHT, and
    // the slice delta below carries that id. Announcing the slice first left it
    // pointing at a pan nothing had registered, so the slice belonged to no
    // pane — which is why click-to-tune reported "Slice capacity is full": the
    // spectrum could not resolve a tune target on a pan it thought was empty,
    // and fell through to the create-a-slice path against a one-slice radio.
    //
    // Provisional geometry: the first 0x27 sweep replaces it a few tens of ms
    // later. A placeholder that is replaced beats an association that never forms.
    emit panCenterBandwidthChanged(panId(), 0.0, 0.0);

    // One slice, and it exists from the moment we connect. Without it nothing
    // downstream has anything to attach audio to — including the TCI receiver
    // channel, which is routed by slice.
    SliceDelta s;
    s.panId = panId();
    s.inUse = true;
    s.active = true;
    s.txSlice = true;   // one receiver IS the transmitter
    if (m_model && m_model->civAddress == 0xB6) {
        s.rxAntennaList = QStringList{QStringLiteral("ANT1"),
                                      QStringLiteral("RX-ANT")};
        s.txAntennaList = QStringList{QStringLiteral("ANT1")};
        s.txAntenna = QStringLiteral("ANT1");
        // The documented read form returns only FB on live B6 firmware, so no
        // current selection is claimed here. A user selection is optimistic
        // for this session; reconnect never replays client-owned state.
    }
    emit sliceChanged(sliceId(), s);

    publishMeterDefs();

    // THE RF GAIN IS A REAL REGISTER, and it is not the preamp.
    //
    // This slider used to drive 16 02 — the three-position preamp — and label
    // its positions "0 dB", "1 dB", "2 dB". None of those is a decibel of
    // anything: the radio calls them OFF, P.AMP1 and P.AMP2 and publishes no
    // gain figures for them. Meanwhile 14 02, the radio's actual continuous RF
    // gain, was not wired at all, so the one control an operator reaches for
    // when a strong band overloads the front end was unreachable.
    //
    // PERCENT, not dB. 14 02 is 0000..0255 with no published dB mapping, so a
    // dB label here would be the same invention in a new place.
    emit panRfGainInfoChanged(panId(), 0, 100, 1, QStringLiteral("%"));

    // The two DISCRETE stages, published as named positions. Their size is the
    // control's range, so a model with a different preamp ladder or a different
    // attenuator step describes itself correctly without a UI change.
    //
    // The preamp collapses to two positions above 50 MHz — the guide says
    // 00/01/02 on HF and 00/01 on 144/430 — and this publishes the HF ladder.
    // Selecting P.AMP2 on 2 m is refused by the radio, which then reports what
    // it actually did; the alternative, republishing on every band change,
    // would rewrite the control under an operator mid-adjustment.
    // PER MODEL, and silent when we do not know. These ladders used to be
    // IC-705 literals emitted to every Icom, so an IC-7610 (multi-step
    // attenuator) or an IC-9700 (different preamp ladder) got a control that
    // misdescribed its own register — the defect class this backend's registry
    // exists to surface, reintroduced by the fix for it. Same rule as
    // powerCurveFor: no verified table means publish nothing, and the operator
    // gets no button rather than a lying one.
    const auto preampLabels = preampLabelsFor(*m_model);
    if (!preampLabels.empty()) {
        QStringList labels;
        for (std::string_view l : preampLabels)
            labels << QString::fromUtf8(l.data(), static_cast<int>(l.size()));
        emit panPreampInfoChanged(panId(), labels);
    }
    // ONE step, and naming it in dB is honest here where it was not for the
    // preamp: the guide gives this attenuator an actual figure. HF and 50 MHz
    // only — on higher bands the radio ignores the request and reports OFF.
    const auto attenSteps = attenStepsFor(*m_model);
    if (!attenSteps.empty()) {
        QStringList labels;
        for (const auto& a : attenSteps)
            labels << QString::fromUtf8(a.label.data(), static_cast<int>(a.label.size()));
        emit panAttenuatorInfoChanged(panId(), labels);
    }

    // A small default set so the status bar is alive before any UI declares
    // what it is showing. setMeterVisible() narrows or widens this.
    m_meters.setVisible(MeterId::SMeter, true);
    m_meters.setVisible(MeterId::Vd, true);
    m_meters.setVisible(MeterId::Overflow, true);
    // The transmit meters. Visible so the poller WILL ask for them — it still
    // only does so while transmitting, which is what the TX/RX split is for.
    m_meters.setVisible(MeterId::Power, true);
    m_meters.setVisible(MeterId::Swr, true);
    m_meters.setVisible(MeterId::Alc, true);
    m_meters.setVisible(MeterId::Comp, true);
    m_meters.setVisible(MeterId::Id, true);

    m_meterTimer = new QTimer(this);
    connect(m_meterTimer, &QTimer::timeout, this, &IcomCivBackend::onMeterTick);
    m_meterTimer->start(kMeterTickMs);

    m_linkTimer = new QTimer(this);
    connect(m_linkTimer, &QTimer::timeout, this, &IcomCivBackend::onLinkTick);
    m_linkTimer->start(kLinkTickMs);

    // Start the first snapshot request now.  Every remaining startup/control/
    // meter request leaves through the same paced writer on timer ticks.
    pumpCiv(nowMs());


}

void IcomCivBackend::onSessionDisconnected(const QString& reason)
{
    const bool was = m_connected;
    m_connected = false;

    // UNKNOWN IS THE ONLY HONEST STARTING POINT for anything the radio told
    // us, and this object outlives the session: the same backend serves the
    // next connect, and a radio swap in one process reaches a DIFFERENT radio.
    // Carrying these over meant Radio Health could print the previous radio's
    // MOD levels beside the new radio's selection, and a surviving
    // m_lastModInputWarning silently swallowed a warning that was still true
    // in the new session because it happened to read the same.
    m_dataOffModInput = -1;
    m_dataModInput = -1;
    m_usbModLevelPercent = -1;
    m_accessoryModLevelPercent = -1;
    m_networkModLevelPercent = -1;
    m_micGainReported = false;
    m_pcAudioEnabled.reset();
    m_dataOffModRestore.reset();
    m_lastModInputWarning.clear();

    if (was)
        emit disconnected();
    if (!reason.isEmpty())
        emit connectionError(reason);
}

void IcomCivBackend::checkModInput()
{
    const auto mod = modulationProfileFor(*m_model);
    if (!mod)
        return;
    const auto name = [&mod](int value) {
        for (const ModulationInputChoice& choice : mod->choices) {
            if (choice.value == value) {
                return QString::fromUtf8(choice.label.data(),
                                         static_cast<int>(choice.label.size()));
            }
        }
        return QStringLiteral("unknown(%1)").arg(value);
    };

    QStringList wrong;
    // ONLY THE "ON" DIRECTION IS THE CLIENT'S BUSINESS. PC Audio on and
    // DATA OFF MOD somewhere else is a real fault: the radio keys and puts no
    // modulation on the air. PC Audio OFF makes no claim at all — the operator
    // is then free to route voice from MIC, USB, ACC or anything else, and
    // asserting an expected value there would be the client telling a working
    // radio it is misconfigured.
    if (m_pcAudioEnabled && *m_pcAudioEnabled && m_dataOffModInput >= 0
        && m_dataOffModInput != mod->networkOnlyValue) {
        wrong << QStringLiteral("PC Audio is on but DATA OFF MOD is %1")
                     .arg(name(m_dataOffModInput));
    }
    if (m_dataMode && m_dataModInput >= 0
        && m_dataModInput != mod->networkOnlyValue) {
        wrong << QStringLiteral("DATA MOD is %1, so generated digital audio is ignored")
                     .arg(name(m_dataModInput));
    }
    if (wrong.isEmpty()) {
        m_lastModInputWarning.clear();
        return;
    }

    // NAME THE REMEDY, because this client deliberately will not apply it
    // unasked: DATA OFF MOD is the radio's to persist (Constitution III), so
    // the fix has to come from the operator. Without the second sentence the
    // advisory describes a fault and leaves them to guess that the button they
    // already have is what corrects it.
    const QString warning =
        QStringLiteral("Icom modulation input: %1. Toggle PC Audio off and on to "
                       "select it, or set it on the radio's front panel "
                       "(MENU > SET > Connectors > MOD Input). Check Radio Health "
                       "for the reported source and level.")
            .arg(wrong.join(QStringLiteral(", ")));
    if (warning != m_lastModInputWarning) {
        m_lastModInputWarning = warning;
        emit configurationWarning(warning);
    }
}

void IcomCivBackend::applyScopeStartup()
{
    if (!m_session || !m_model->hasScope || m_scopeStarted)
        return;
    // ONCE PER SESSION, because it is now called from two places. The connect
    // edge runs it for a radio the handshake name already resolved; a radio
    // resolved LATE — only by its 0x19 0x00 address, which is the case
    // auto-detect newly makes reachable — would otherwise never have its scope
    // switched on at all, and would show the black panadapter this function
    // exists to prevent.
    m_scopeStarted = true;
    // BOTH switches. Enabling only 0x27 0x10 turns the scope on the radio's own
    // screen and sends us nothing — the number-one "black panadapter" cause.
    queueWrite(cmdScopeOnOff(m_session->civAddress(), true), "scope.on",
               IcomCivScheduler::Priority::Maintenance, false);
    queueWrite(cmdScopeDataOutput(m_session->civAddress(), true), "scope.output",
               IcomCivScheduler::Priority::Maintenance, false);
}

// ---------------------------------------------------------------------------
// CI-V decode
// ---------------------------------------------------------------------------

void IcomCivBackend::onCivFrame(const CivFrame& frame)
{
    // Scope first: it is by far the highest-rate frame, and the decoder already
    // rejects anything that is not waveform data.
    if (auto sweep = m_scope.feed(frame)) {
        ScopeGeometry geom;
        geom.points = m_model->scopePoints ? m_model->scopePoints : kScopePointsIc705;
        geom.maxAmplitude = m_model->scopeMaxAmplitude ? m_model->scopeMaxAmplitude
                                                       : kScopeMaxAmplitude;
        // THE RADIO'S OWN GEOMETRY, kept so the pan intents below have something
        // true to reason against. Both of them need it: a zoom step has to know
        // which of the eight spans it is leaving, and a centre request has to
        // know what to snap the view back to.
        if (sweep->bandwidthHz() > 0) {
            m_scopeCentreHz = sweep->centreHz();
            m_scopeSpanHz   = sweep->bandwidthHz() / 2;
        }
        emit panCenterBandwidthChanged(panId(),
                                       static_cast<double>(sweep->centreHz()) / 1e6,
                                       static_cast<double>(sweep->bandwidthHz()) / 1e6);
        emit spectrumFrameReady(0, floatBytes(toDbm(*sweep, geom, m_scopeCal)));
        return;
    }

    const qint64 frameAtMs = nowMs();
    ++m_framesObserved;
    m_lastInboundCivAtMs = frameAtMs;

    // PAST THE SCOPE RETURN, so sweeps never enter the ring. Re-serialised
    // rather than captured raw because the parsed frame is what we have here,
    // and for diagnosis the envelope is noise — the command bytes are the
    // evidence. Terminator included so an FB/FA reply is unmistakable.
    {
        std::vector<std::uint8_t> flat;
        flat.reserve(frame.data.size() + 4);
        flat.push_back(frame.cmd);
        if (frame.hasSub)
            flat.push_back(frame.sub);
        flat.insert(flat.end(), frame.data.begin(), frame.data.end());
        const bool routine = frame.cmd == cmd::kMeter
            || (frame.cmd == cmd::kControl && frame.hasSub && frame.sub == control::kPtt);
        traceCiv(/*outbound=*/false, flat, routine);
    }

    // A bus echo is not a reply. IcomSession normally removes it, but keep the
    // scheduler from retiring an identity transaction if an echo reaches this
    // seam: the echoed 19 00 has the same command shape as the real answer.
    if (frame.cmd == cmd::kReadId && frame.from == kControllerAddress)
        return;

    const IcomCivScheduler::Observation observation =
        m_civScheduler.observe(frame, frameAtMs);
    // Identity can retarget the CI-V destination. Do not dispatch the next
    // queued startup frame until adoptReportedCivAddress() has either confirmed
    // the address or discarded all work aimed at the old one.
    const bool identityReply = frame.cmd == cmd::kReadId;
    if (!identityReply)
        pumpCiv(frameAtMs);
    const bool isPttState = frame.cmd == cmd::kControl && frame.hasSub
        && frame.sub == control::kPtt && !frame.data.empty();
    if (observation == IcomCivScheduler::Observation::Stale && !isPttState) {
        qCWarning(lcIcomScheduler)
            << "suppressed stale CI-V completion" << frame.cmd << frame.sub;
        if (identityReply)
            pumpCiv(frameAtMs);
        return;
    }

    noteControlSeen(frame.cmd, frame.sub, frame.hasSub);

    switch (frame.cmd) {
    case cmd::kReadId: {
        if (auto addr = parseModelIdReply(frame)) {
            // THE ADDRESS ARRIVES TWICE — in the frame's `from` byte and in the
            // payload — and they agreed on every measured run. Prefer the
            // payload, because that is what the command is defined to answer,
            // but say so when they differ rather than silently picking one: a
            // disagreement means something is rewriting frames between the radio
            // and us, and that is worth knowing before it is diagnosed as a
            // wrong address.
            if (frame.from != 0 && frame.from != *addr) {
                qCWarning(lcIcomAddr) << "0x19 0x00 reply disagrees with itself: from"
                                  << Qt::hex << frame.from << "payload" << *addr
                                  << "- using the payload";
            }
            adoptReportedCivAddress(*addr);
            // AMBIGUOUS BUS: two devices answered with different addresses, so
            // neither one's identity can be trusted either. Leave m_model where
            // the name put it.
            if (m_civAmbiguous) {
                pumpCiv(frameAtMs);
                return;
            }
            if (const IcomModel* m = modelForCivAddress(*addr)) {
                m_model = m;
                // The span limits and scope geometry are model facts, so they
                // can only be published once the radio has named itself.
                const auto widths = availableBandwidthsHz();
                if (!widths.empty() && m_model->hasScope)
                    emit panBandwidthLimitsChanged(panId(), widths.front() / 1e6,
                                                   widths.back() / 1e6);

                // ⛔ Publish the Y axis too, or the display invents one and
                // never stops. Without a range from the backend the pan
                // auto-ranges from its own noise-floor estimate, and because
                // MainWindow refuses anything below -180 dBm
                // (dbmRangeLooksPlausible) the radio never adopts the value —
                // so the estimate is never corrected and drifts further every
                // cycle. Observed on a live IC-9700 2026-08-05: a linear
                // runaway of -24 dB/s, 84 rejected `display pan set` commands
                // in 90 s, min falling -202 -> -898 dBm and still going. The
                // operator sees the waterfall reset each time the drift crosses
                // the guard, and the radio menu stops responding behind the
                // command flood.
                //
                // The numbers are m_scopeCal's own — ESTIMATES, as its header
                // says at length, not a measurement. Publishing an estimate is
                // right here: the axis is anchored and stable, and the estimate
                // is already the one toDbm() decodes with, so the display and
                // the decoder agree. An uncalibrated-but-consistent axis beats
                // a self-referential one.
                publishScopeDbmRange();

                publishMeterDefs();
                publishCapabilities();
                // The scope switches are per-model, so a radio that only became
                // known just now has not had them sent. No-op once started.
                applyScopeStartup();
            }
            RadioDelta r;
            r.model = QString::fromUtf8(m_model->name.data(),
                                        static_cast<int>(m_model->name.size()));
            emit radioChanged(r);
        }
        pumpCiv(frameAtMs);
        return;
    }

    case cmd::kReadFreq:
    case cmd::kSetFreqTrx: {
        // 0x00 is the TRANSCEIVE push the radio sends unprompted when the
        // operator turns the dial; 0x03 is the answer to our poll. Same payload,
        // and both are the truth — which is why they share a case.
        if (auto hz = decodeFreq(frame.data)) {
            m_frequencyHz = *hz;
            SliceDelta s;
            s.frequency = static_cast<double>(*hz) / 1e6;
            emit sliceChanged(sliceId(), s);
            // TxApplet's ATU toggle is deliberately frequency-aware: a second
            // click bypasses only the match made at the current TX frequency.
            // Icom has one VFO here, so publish that same authoritative dial
            // frequency on the transmit model instead of leaving it at 0 MHz.
            TransmitDelta t;
            t.transmitFreq = s.frequency;
            emit transmitChanged(t);
        }
        return;
    }

    case cmd::kReadMode:
    case cmd::kSetModeTrx: {
        if (frame.data.empty())
            return;
        m_mode = static_cast<CivMode>(frame.data[0]);
        // THE SECOND BYTE IS THE FILTER SLOT (1..3), and it was being discarded.
        // It is the only way to know which of the three IF filters is in use —
        // the radio cannot report a passband in Hz — so without it the window
        // was drawn from a per-mode default and never followed the operator
        // changing the filter on the radio's own front panel.
        if (frame.data.size() >= 2 && frame.data[1] >= 1 && frame.data[1] <= 3)
            m_filter = frame.data[1];
        // ASK WHETHER THIS MODE IS A DATA MODE, because the frame that just
        // arrived cannot say. 0x01 is the unsolicited push the radio sends when
        // the operator turns the MODE knob, and USB→USB-D on the front panel
        // produces exactly the same 01 01 xx as USB→USB. Nothing else in the
        // protocol announces that change, so following it means asking — and
        // 0x26 is the only command that can answer.
        //
        // Event-driven, not a timer: one read per front-panel mode change, on
        // the unsolicited form only. Answering our own 04 poll with another
        // read would be a second poll of a state the connect snapshot and this
        // path already cover, and the confirmation read in setSliceMode covers
        // app-originated changes.
        // Do not publish a capable radio's 04/01 frame: it cannot refresh
        // m_dataMode, so combining it with the new ordinary mode would expose a
        // transient false DIGU/DIGL (or false voice mode) until 26 answered.
        // Command 26 is the single authoritative publication for these models.
        if (frame.cmd == cmd::kSetModeTrx && m_session && m_model->hasVfoModeCommand) {
            const auto read = cmdReadVfoMode(m_session->civAddress());
            queueRead(read, semanticKey(read), IcomCivScheduler::Priority::Maintenance);
            pumpCiv(nowMs());
        } else if (!m_model->hasVfoModeCommand) {
            // This model has no verified DATA readback. An ordinary mode frame
            // can only justify an ordinary mode claim.
            m_dataMode = false;
            publishModeState();
        }
        return;
    }

    // THE RADIO'S OWN LEVELS AND SWITCHES, adopted into the models.
    //
    // These arrive as answers to the connect-time reads above, and also
    // unsolicited whenever the operator turns a knob on the radio — the same
    // decode serves both, which is what keeps the UI honest while someone is
    // standing at the rig.
    //
    // EVERY DECODE ALSO ADOPTS INTO THE SCRUB MIRROR. The "last intent per
    // control" block in the header is what `controls.scrub` re-asserts, and it
    // was written ONLY by the setters — so on a session where the operator had
    // touched nothing, the mirrors still held their construction defaults and a
    // scrub documented as leaving the radio untouched drove RF gain to 0 (a
    // deaf receiver), AF gain to 0, the preamp and attenuator off and AGC to
    // MID, then reported every one of those rows LINKED because the intent did
    // reach the wire. Same shape as the noise-reduction bug fixed earlier on
    // this branch, on a dozen sibling rows. The header's own claim — "a radio
    // that disagrees corrects these through the ordinary decode path" — is what
    // these assignments make true.
    case cmd::kLevel: {
        if (!frame.hasSub)
            return;
        const auto raw = decodeLevel(frame.data);
        if (!raw)
            return;
        // Match the radio's own integer display buckets. Nearest rounding made
        // RF power, mic gain, monitor level, and every sibling percentage read
        // one point ahead of the front panel for roughly half their range.
        const int pct = levelRawToPercent(*raw);
        switch (frame.sub) {
        case level::kRfPower: {
            m_txPowerPercent = pct;
            TransmitDelta t; t.rfPower = pct;
            emit transmitChanged(t);
            return;
        }
        case level::kMicGain: {
            m_micGainPercent = pct;
            m_micGainReported = true;
            TransmitDelta t; t.micLevel = pct;
            emit transmitChanged(t);
            return;
        }
        case level::kCompLevel: {
            // The radio's 0..10 compressor mapped back onto NOR/DX/DX+.
            m_compLevelPercent = pct;
            TransmitDelta t;
            t.speechProcLevel = pct;
            emit transmitChanged(t);
            return;
        }
        case level::kAf: {
            m_afGainPercent = pct;
            SliceDelta d; d.audioGain = pct;
            emit sliceChanged(sliceId(), d);
            return;
        }
        case level::kSquelch: {
            m_squelchPercent = pct;
            SliceDelta d;
            d.squelchLevel = pct;
            // NO SEPARATE ENABLE on this radio — the threshold IS the control,
            // so a non-zero threshold is what "squelch on" means here.
            d.squelchOn = pct > 0;
            emit sliceChanged(sliceId(), d);
            return;
        }
        case level::kNrLevel: {
            m_nrLevelPercent = pct;
            SliceDelta d; d.nrLevel = pct;
            emit sliceChanged(sliceId(), d);
            return;
        }
        case level::kNbLevel: {
            m_nbLevelPercent = pct;
            SliceDelta d; d.nbLevel = pct;
            emit sliceChanged(sliceId(), d);
            return;
        }
        case level::kNotchPos: {
            m_notchPosPercent = pct;
            SliceDelta d; d.mnLevel = pct;
            emit sliceChanged(sliceId(), d);
            return;
        }
        case level::kRf: {
            m_rfGainPercent = pct;
            emit panRfGainChanged(panId(), pct);
            return;
        }
        case level::kVoxGain: {
            m_voxLevelPercent = pct;
            TransmitDelta t; t.voxLevel = pct;
            emit transmitChanged(t);
            return;
        }
        case level::kMonitor: {
            m_monitorLevelPercent = pct;
            TransmitDelta t; t.monGainSb = pct;
            emit transmitChanged(t);
            return;
        }
        default:
            return;
        }
    }

    case cmd::kFunction: {
        if (!frame.hasSub || frame.data.empty())
            return;
        const int v = frame.data[0];
        switch (frame.sub) {
        case func::kNoiseReduce: {
            m_nrEnableSent = v ? 1 : 0;   // adopt, so we do not re-send it
            SliceDelta d; d.nr = (v != 0);
            emit sliceChanged(sliceId(), d);
            return;
        }
        case func::kNoiseBlanker: {
            m_nbEnableSent = v ? 1 : 0;
            SliceDelta d; d.nb = (v != 0);
            emit sliceChanged(sliceId(), d);
            return;
        }
        case func::kAutoNotch: {
            m_anfEnableSent = v ? 1 : 0;
            SliceDelta d; d.anf = (v != 0);
            emit sliceChanged(sliceId(), d);
            return;
        }
        case func::kManualNotch: {
            m_mnEnableSent = v ? 1 : 0;
            SliceDelta d; d.mn = (v != 0);
            emit sliceChanged(sliceId(), d);
            return;
        }
        case func::kMonitorFn: {
            // Was read at connect and dropped through this switch's default, so
            // the monitor button opened at OUR default on a radio that may have
            // had it on.
            m_monitorSent = v ? 1 : 0;
            m_monitorOn = (v != 0);
            TransmitDelta t; t.sbMonitor = (v != 0);
            emit transmitChanged(t);
            return;
        }
        case func::kVox: {
            // Same story: asked for at connect, answer discarded. A read whose
            // reply is thrown away is pure cost on a shared stream.
            m_voxEnableSent = v ? 1 : 0;
            m_voxOn = (v != 0);
            TransmitDelta t; t.voxEnable = m_voxOn;
            emit transmitChanged(t);
            return;
        }
        case func::kCompressor: {
            m_compEnable = (v != 0);
            TransmitDelta t; t.speechProcEnable = (v != 0);
            emit transmitChanged(t);
            return;
        }
        case func::kAgc: {
            // 01 FAST, 02 MID, 03 SLOW.
            SliceDelta d;
            d.agcMode = v == 1 ? QStringLiteral("fast")
                      : v == 3 ? QStringLiteral("slow")
                               : QStringLiteral("med");
            m_agcMode = *d.agcMode;
            emit sliceChanged(sliceId(), d);
            return;
        }
        case func::kPreamp: {
            // The PREAMP control, not the RF-gain slider. It used to publish
            // into SliceDelta::rfGain, which is what made a three-position
            // switch look like a gain reading.
            m_preampStep = std::clamp(v, 0, 2);
            emit panPreampChanged(panId(), m_preampStep);
            return;
        }
        default:
            return;
        }
    }

    case cmd::kAttenuator: {
        // 11 <bcd dB>. Anything non-zero is the attenuator's one engaged
        // position; the dB figure is decoded rather than assumed so a model
        // with more than one step still lands on "not off".
        if (frame.data.empty())
            return;
        const int db = decodeBcdByte(frame.data[0]);
        // Map the reported dB back through the SAME table the setter sends
        // from, so a model with more than one step lands on the right position
        // instead of collapsing to "not off". Unrecognised dB falls back to
        // that collapse, which is still better than reporting OFF.
        const auto steps = attenStepsFor(*m_model);
        int reported = db > 0 ? 1 : 0;
        for (std::size_t i = 0; i < steps.size(); ++i) {
            if (steps[i].db == db) {
                reported = static_cast<int>(i);
                break;
            }
        }
        m_attenStep = reported;
        emit panAttenuatorChanged(panId(), reported);
        return;
    }

    // 26 00 <mode> <data> <filter> — MODE, DATA STATE AND FILTER TOGETHER.
    //
    // This case is what makes a front-panel USB-D visible. Mode byte 0x01 is
    // USB whether or not DATA is on, so until this decoded, a radio the
    // operator had put in USB-D read as plain USB indefinitely — and every
    // AetherSDR decision that follows from the mode name (the indicator, the
    // passband, whether the mod-input warning applies) was taken for the wrong
    // mode.
    //
    // RADIO-AUTHORITATIVE (Constitution II): this OVERWRITES whatever
    // setSliceMode optimistically assumed. The optimistic value exists only to
    // fill the gap until this arrives; when the two disagree the radio is
    // right, including when the radio simply refused the change.
    case cmd::kVfoMode: {
        // THE SELECTED VFO ONLY. A reply for the unselected one describes a VFO
        // the app does not model, and adopting it would publish the other VFO's
        // mode on the slice the operator is listening to.
        if (!frame.hasSub || frame.sub != vfoMode::kSelected)
            return;
        const auto st = decodeVfoMode(frame.data);
        if (!st)
            return;
        m_mode = st->mode;
        m_dataMode = st->dataMode;
        // Zero means the radio named a slot outside 1..3 — see VfoModeState.
        // Keeping the previous slot is what stops mode and filter clobbering
        // each other, which is the whole reason the three travel in one frame.
        if (st->filter != 0)
            m_filter = st->filter;
        publishModeState();
        checkModInput();
        return;
    }

    case cmd::kSetting: {
        // 1A 05 <item hi> <item lo> <value>
        if (!frame.hasSub || frame.sub != 0x05 || frame.data.size() < 3)
            return;
        const int item = decodeBcdByte(frame.data[0]) * 100 + decodeBcdByte(frame.data[1]);
        const auto mod = modulationProfileFor(*m_model);
        if (!mod)
            return;
        if (item == mod->dataOffInputItem) {
            m_dataOffModInput = frame.data[2];
        } else if (item == mod->dataInputItem) {
            m_dataModInput = frame.data[2];
        } else {
            const auto raw = decodeLevel(std::span(frame.data).subspan(2));
            if (!raw)
                return;
            const int pct = levelRawToPercent(*raw);
            if (item == mod->usbLevelItem) {
                m_usbModLevelPercent = pct;
            } else if (item == mod->accessoryLevelItem) {
                m_accessoryModLevelPercent = pct;
            } else if (item == mod->networkLevelItem) {
                m_networkModLevelPercent = pct;
            } else {
                return;
            }
        }
        checkModInput();
        return;
    }

    case cmd::kMeter: {
        if (!frame.hasSub)
            return;
        const MeterSpec* spec = meterSpecForSub(frame.sub);
        if (!spec)
            return;

        // OVF IS ONE BYTE, not a two-byte BCD level.
        //
        // 15 07 answers 00 or 01 — a flag, not a reading — and decodeLevel
        // rejects anything shorter than two bytes. So every ADC-overflow reply
        // was dropped before markAnswered, the poller re-asked on the in-flight
        // timeout forever, and the indicator that tells an operator they are
        // clipping the converter never moved once. `controls meters` reported it
        // as NEVER FED with the replies plainly visible in `civ trace` — which
        // is the whole reason to measure a meter's age rather than its
        // definition.
        std::optional<int> raw = spec->id == MeterId::Overflow
            ? (frame.data.empty() ? std::nullopt
                                  : std::optional<int>(frame.data[0] != 0 ? 1 : 0))
            : decodeLevel(frame.data);
        if (!raw)
            return;

        m_meters.markAnswered(spec->id, nowMs());
        const double value = meterValue(spec->id, *raw,
                                        s9ReferenceFor(m_frequencyHz),
                                        m_model ? m_model->civAddress : 0xA4);

        if (spec->id == MeterId::Overflow) {
            m_overflow = value > 0.5;
        } else if (spec->id == MeterId::Vd) {
            m_vdVolts = value;
        } else if (spec->id == MeterId::Id) {
            m_idAmps = value;
        }
        // "SOURCE:NAME", the id every consumer looks up by. Emitting the bare
        // name published a meter nothing could find: radiocert's inventory
        // reported SLC:LEVEL as never defined while the S-meter was decoding
        // correctly the whole time — the orphaned-meter-seam defect, again.
        emit meterUpdate(QStringLiteral("%1:%2")
                             .arg(QString::fromUtf8(spec->source.data(),
                                                    static_cast<int>(spec->source.size())),
                                  QString::fromUtf8(spec->name.data(),
                                                    static_cast<int>(spec->name.size()))),
                         value);
        return;
    }

    case cmd::kControl: {
        if (frame.hasSub && frame.sub == control::kPtt && !frame.data.empty()) {
            const bool keyed = frame.data[0] != 0;
            // A read can already be on the wire when the operator keys.  Its
            // pre-write OFF answer then arrives after the newer ON request.
            // During the bounded confirmation window only the requested value
            // may confirm the intent; a contradictory value is diagnostic
            // history, not a newer state transition.  Once the window expires,
            // the next fresh radio report wins again (Constitution II).
            //
            // ONE DIRECTION ONLY — suppression applies while the pending intent
            // is KEY ON, never while it is key off.  The two directions are not
            // symmetric risks.  Swallowing a stale OFF after a key-on request
            // costs a transmission (the captured FT8 failure).  Swallowing an
            // unexpected ON after an unkey request costs the operator any
            // indication that the radio is still on the air — when the unkey was
            // lost, refused, or overridden at the front panel, that report is
            // the only thing that says so.  RFC #4983 states the rule directly:
            // "Explicit PTT OFF and fail-safe unkey are never suppressed by a
            // key-on transition guard", and Constitution VI wants every path
            // that can transmit to fail closed.
            if (m_pendingPttIntent) {
                const bool confirmsIntent = keyed == *m_pendingPttIntent;
                const bool guarding = *m_pendingPttIntent
                    && !confirmsIntent && frameAtMs < m_pendingPttUntilMs;
                if (guarding) {
                    qCWarning(lcIcomScheduler)
                        << "suppressed contradictory PTT state during confirmation"
                        << "reported" << keyed << "intent" << *m_pendingPttIntent;
                    return;
                }
                if (!confirmsIntent && !*m_pendingPttIntent) {
                    // The radio says it is keyed while we asked it to stop.
                    // Publish it and say so — this is the fail-closed path.
                    qCWarning(lcIcomScheduler)
                        << "radio reports KEYED after an unkey request; "
                           "publishing radio truth";
                }
                m_pendingPttIntent.reset();
                m_pendingPttUntilMs = 0;
            } else if (observation == IcomCivScheduler::Observation::Stale) {
                return;
            }
            // ON CHANGE ONLY. This is the answer to a poll that runs four times
            // a second, and it used to republish the transmit state on every
            // one of them — a 4 Hz stream of "the radio is transmitting" events
            // riding on top of every transmission, each re-applied through
            // TransmitModel and everything downstream of it.
            //
            // Republishing unchanged state is never merely wasteful on a path
            // this hot: it is indistinguishable, to every consumer, from the
            // state having just changed.
            if (keyed == m_keyed)
                return;
            m_keyed = keyed;
            m_meters.setTransmitting(m_keyed);
            TransmitDelta t;
            t.mox = m_keyed;
            emit transmitChanged(t);
            return;
        }
        if (frame.hasSub && frame.sub == control::kTuner && !frame.data.empty()) {
            // 00 off, 01 on (matched), 02 mid-cycle. Reported as the neutral
            // tokens TunerModel's ATUStatus parse already understands, so the
            // ATU button's three states come from the radio rather than from
            // our own guess about how long a cycle takes.
            const int v = frame.data[0];
            TransmitDelta t;
            // Apply frequency and tuner status in ONE delta. That makes the
            // successful-state callback capture the right frequency even if a
            // tuner reply overtakes the separate connect-time frequency read.
            if (m_frequencyHz > 0)
                t.transmitFreq = static_cast<double>(m_frequencyHz) / 1e6;
            t.atuEnabled = (v != 0);
            t.atuStatusRaw = v == 0x02 ? QStringLiteral("TUNE_IN_PROGRESS")
                           : v == 0x01 ? QStringLiteral("TUNE_SUCCESSFUL")
                                       : QStringLiteral("TUNE_BYPASS");
            emit transmitChanged(t);
        }
        return;
    }

    case cmd::kTuneOffset: {
        if (!frame.hasSub)
            return;
        if (frame.sub == tuneOffset::kRitOnOff && !frame.data.empty()) {
            m_ritOn = frame.data[0] != 0;
            SliceDelta d; d.ritOn = m_ritOn;
            emit sliceChanged(sliceId(), d);
            return;
        }
        if (frame.sub == tuneOffset::kXitOnOff && !frame.data.empty()) {
            m_xitOn = frame.data[0] != 0;
            SliceDelta d; d.xitOn = m_xitOn;
            emit sliceChanged(sliceId(), d);
            return;
        }
        if (frame.sub == tuneOffset::kFrequency && frame.data.size() >= 3) {
            // Two BCD bytes little-endian holding 0000..9999 Hz, then a SIGN
            // byte (00 plus, 01 minus). Folding the sign into the magnitude
            // reads the offset backwards, which is the same mistake the encode
            // side documents.
            const int lo = decodeBcdByte(frame.data[0]);
            const int hi = decodeBcdByte(frame.data[1]);
            int hz = hi * 100 + lo;
            if (frame.data[2] != 0)
                hz = -hz;
            // ONE REGISTER, BOTH CONTROLS. 21 01 / 21 02 choose whether it
            // applies to receive, transmit or both, so the same offset is
            // published to each — a slice that showed RIT 0 while the radio was
            // offset is exactly the reconnect bug this read exists to close.
            m_ritOffsetHz = hz;
            SliceDelta d;
            d.ritFreq = hz;
            d.xitFreq = hz;
            emit sliceChanged(sliceId(), d);
        }
        return;
    }

    default:
        return;
    }
}

// ---------------------------------------------------------------------------
// Audio — the path WSJT-X depends on
// ---------------------------------------------------------------------------

void IcomCivBackend::onAudio(const std::vector<float>& mono)
{
    if (mono.empty() || !m_rxResampler)
        return;

    // 48 kHz MONO from the radio -> 24 kHz interleaved STEREO for the engine.
    //
    // This one line is the whole TCI/WSJT-X path. The seam's per-slice contract
    // is interleaved stereo float32 at 24 kHz — Hl2RxDsp::audioReady names it
    // `stereoPcm` and TciServer constructs its resampler with a 24000 source
    // rate — and the radio hands us neither. Skipping the rate conversion plays
    // back an octave low; skipping the channel duplication feeds TciServer half
    // the frames it thinks it has, because it divides by 2*sizeof(float).
    const QByteArray stereo24k =
        m_rxResampler->processMonoToStereo(mono.data(), static_cast<int>(mono.size()));
    if (stereo24k.isEmpty())
        return;

    // The speaker feed.
    emit audioFrameReady(stereo24k);

    // And the PER-SLICE feed, which is a different consumer and not optional:
    // the TCI receiver channels are routed by slice, because a mixed feed
    // cannot say which slice a buffer belongs to. This is the signal that ends
    // up as TCI audio channel 1 for WSJT-X.
    //
    // Emitted PRE-mute and PRE-gain by contract — muting a slice must silence
    // the monitor without stopping a decoder that is running on it.
    emit sliceAudioFrameReady(sliceId(), stereo24k);
}

void IcomCivBackend::submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz,
                                   bool clientLeveled)
{
    // The flag is the HL2's concern: this backend ships PCM to a radio that
    // runs its own transmit processing, so there is no host ALC here to bypass.
    Q_UNUSED(clientLeveled);
    if (!m_session || !m_connected)
        return;

    // ONLY WHILE KEYED — and the engine is relying on us for this.
    //
    // AudioEngine deliberately does NOT PTT-gate the tap that feeds this
    // ("No PTT gate here: Hl2Backend::submitTxAudio drops audio unless keyed"),
    // because the seam contract puts the gate in the backend. This one had no
    // gate at any layer: not here, not in IcomSession::sendAudio, and not in
    // onTxPump. So the operator's live microphone streamed into the radio's
    // WLAN modulation input for the entire session.
    //
    // Two things that costs, and the first is a transmit-safety question. A
    // radio with VOX enabled keys on that feed, with no intent expressed
    // anywhere in this client — and this backend can neither read nor clear VOX
    // (Principle VI: nothing automates into a keyed transmitter). The second is
    // that TxPacketizer caps at 250 ms and drops the OLDEST on overflow, so a
    // continuously-fed queue saturates and then sheds periodically.
    //
    // SAFE TO GATE, because the audio stream does not depend on this traffic to
    // stay up: IcomStream runs its own idle and ping timers, and RS-BA1's
    // keepalive is the 0x00 idle packet rather than the audio payload. Stopping
    // audio between overs stops audio, not the session.
    //
    // m_tuning is included because a TUNE carrier is synthesised in place of
    // this buffer further down and must still reach the radio.
    if (!m_keyed && !m_tuning) {
        return;
    }
    // The engine hands us interleaved int16 stereo; the radio wants mono at its
    // negotiated rate. Downmix here rather than in IcomSession so the session
    // stays a transport.
    const int frames = static_cast<int>(int16Stereo.size() / (2 * sizeof(qint16)));
    if (frames <= 0)
        return;
    const auto* src = reinterpret_cast<const qint16*>(int16Stereo.constData());
    std::vector<float> mono(static_cast<std::size_t>(frames));
    if (m_tuning) {
        // A TUNE carrier, synthesised in place of whatever the engine sent.
        // Phase is carried across buffers: restarting it each block would put a
        // discontinuity at the block rate, which is a click every few
        // milliseconds and splatter either side of the carrier.
        const double step = 2.0 * M_PI * kTuneToneHz / static_cast<double>(sampleRateHz);
        for (int i = 0; i < frames; ++i) {
            mono[static_cast<std::size_t>(i)] =
                kTuneToneAmplitude * static_cast<float>(std::sin(m_tunePhase));
            m_tunePhase += step;
            if (m_tunePhase > 2.0 * M_PI)
                m_tunePhase -= 2.0 * M_PI;
        }
    } else {
        for (int i = 0; i < frames; ++i)
            mono[static_cast<std::size_t>(i)] =
                (src[i * 2] + src[i * 2 + 1]) * 0.5f / 32768.0f;
    }

    // RESAMPLE, don't refuse.
    //
    // This used to drop every buffer whose rate was not already the radio's,
    // on the reasoning that converting silently would hide a mismatch. That was
    // backwards: the seam's transmit contract IS 24 kHz (AudioEngine::
    // DEFAULT_SAMPLE_RATE) and this radio's stream is 48 kHz, so converting is
    // the job — exactly as the receive path already converts 48 kHz down to 24.
    // Refusing turned a known, expected rate difference into a transmitter that
    // keyed and sent nothing.
    if (sampleRateHz != m_audioRateHz) {
        if (sampleRateHz <= 0)
            return;
        // Built once and kept: r8brain is stateful, and a fresh instance per
        // buffer restarts its filter history every block — audible as a tick at
        // the block rate, and on a transmit path that goes on the air.
        if (!m_txResampler || m_txResamplerFromHz != sampleRateHz
            || m_txResamplerToHz != m_audioRateHz) {
            m_txResamplerToHz = m_audioRateHz;
            m_txResamplerFromHz = sampleRateHz;
            m_txResampler = std::make_unique<Resampler>(
                static_cast<double>(sampleRateHz),
                static_cast<double>(m_audioRateHz), 4096);
        }
        const QByteArray out =
            m_txResampler->process(mono.data(), static_cast<int>(mono.size()));
        if (out.isEmpty())
            return;
        const auto* f = reinterpret_cast<const float*>(out.constData());
        mono.assign(f, f + out.size() / static_cast<int>(sizeof(float)));
    }
    m_session->sendAudio(mono);
}

// ---------------------------------------------------------------------------
// Intents DOWN
// ---------------------------------------------------------------------------

std::string IcomCivBackend::semanticKey(std::span<const std::uint8_t> frame) const
{
    const std::optional<CivFrame> parsed = parseFrame(frame);
    if (!parsed) {
        return {};
    }
    switch (parsed->cmd) {
    case cmd::kSetFreqTrx:
    case cmd::kReadFreq:
    case cmd::kSetFreq:
        return "frequency";
    case cmd::kSetModeTrx:
    case cmd::kReadMode:
    case cmd::kSetMode:
    case cmd::kVfoMode:
        return "mode";
    default:
        break;
    }
    if (parsed->cmd == cmd::kControl && parsed->hasSub) {
        if (parsed->sub == control::kPtt) {
            return "ptt";
        }
        if (parsed->sub == control::kTuner) {
            return "tuner";
        }
    }
    std::string key = "civ." + std::to_string(parsed->cmd);
    if (parsed->hasSub) {
        key += "." + std::to_string(parsed->sub);
    }
    // SET-menu reads share 1A 05 but name their leaf in the first two data
    // bytes.  Keep the leaves separate so one startup query cannot coalesce a
    // different setting merely because their outer command matches.
    if (parsed->cmd == cmd::kSetting && parsed->hasSub && parsed->data.size() >= 2) {
        key += "." + std::to_string(parsed->data[0]);
        key += "." + std::to_string(parsed->data[1]);
    }
    return key;
}

std::optional<std::vector<std::uint8_t>>
IcomCivBackend::confirmationFor(std::span<const std::uint8_t> frame) const
{
    const std::optional<CivFrame> parsed = parseFrame(frame);
    if (!parsed || parsed->data.empty()) {
        return std::nullopt;
    }
    const std::uint8_t addr = m_session ? m_session->civAddress() : 0xA4;
    switch (parsed->cmd) {
    case cmd::kSetFreq:
        return cmdReadFrequency(addr);
    case cmd::kSetMode:
        return cmdReadMode(addr);
    case cmd::kVfoMode:
        return cmdReadVfoMode(addr);
    case cmd::kLevel:
    case cmd::kFunction:
    case cmd::kControl:
    case cmd::kTuneOffset:
        if (parsed->hasSub) {
            return buildFrameSub(addr, parsed->cmd, parsed->sub);
        }
        break;
    case cmd::kSetting:
        if (parsed->hasSub && parsed->sub == 0x05 && parsed->data.size() >= 3) {
            const int item = decodeBcdByte(parsed->data[0]) * 100
                + decodeBcdByte(parsed->data[1]);
            return cmdReadSetting(addr, item);
        }
        break;
    case cmd::kAttenuator:
        return cmdReadAttenuator(addr);
    default:
        break;
    }
    return std::nullopt;
}

void IcomCivBackend::queueRead(const std::vector<std::uint8_t>& frame,
                               const std::string& key,
                               IcomCivScheduler::Priority priority,
                               qint64 notBeforeMs)
{
    const std::optional<CivFrame> parsed = parseFrame(frame);
    if (!parsed) {
        return;
    }
    IcomCivScheduler::Request request;
    request.frame = frame;
    request.key = key.empty() ? semanticKey(frame) : key;
    request.priority = priority;
    request.expectsReply = true;
    request.replyCmd = parsed->cmd;
    request.replyHasSub = parsed->hasSub;
    request.replySub = parsed->sub;
    request.notBeforeMs = notBeforeMs;
    m_civScheduler.enqueue(std::move(request), nowMs());
}

void IcomCivBackend::queueWrite(const std::vector<std::uint8_t>& frame,
                                const std::string& key,
                                IcomCivScheduler::Priority priority,
                                bool supersedes)
{
    IcomCivScheduler::Request request;
    request.frame = frame;
    request.key = key.empty() ? semanticKey(frame) : key;
    request.priority = priority;
    request.expectsReply = true;
    request.acceptsGenericReply = true;
    request.supersedes = supersedes;
    m_civScheduler.enqueue(std::move(request), nowMs());
}

void IcomCivBackend::queueEmergencyWriteNoReply(const std::vector<std::uint8_t>& frame,
                                                const std::string& key)
{
    IcomCivScheduler::Request request;
    request.frame = frame;
    request.key = key.empty() ? semanticKey(frame) : key;
    request.priority = IcomCivScheduler::Priority::Emergency;
    request.supersedes = true;
    request.coalesce = false;
    m_civScheduler.enqueue(std::move(request), nowMs());
}

void IcomCivBackend::pumpCiv(qint64 nowMs)
{
    if (!m_session || !m_connected) {
        serviceSchedulerWaiters(nowMs);
        return;
    }
    const std::optional<IcomCivScheduler::Dispatch> dispatch = m_civScheduler.takeNext(nowMs);
    if (!dispatch) {
        serviceSchedulerWaiters(nowMs);
        return;
    }
    // ROUTINE = the high-rate loops only.  `>= Ptt` also swept up Control and
    // Maintenance, which hid the startup snapshot and the scope on/output
    // writes from the default `civ trace` — the frames behind the documented
    // number-one "black panadapter" cause. Before the scheduler every outbound
    // frame was shown; keep it that way for everything but the pollers.
    const bool routineDispatch = dispatch->priority == IcomCivScheduler::Priority::Ptt
        || dispatch->priority == IcomCivScheduler::Priority::ActiveMeter;
    traceCiv(/*outbound=*/true, dispatch->frame, routineDispatch);
    if (dispatch->supersedes) {
        if (dispatch->frame.size() > 5) {
            noteControlSent(dispatch->frame[4], dispatch->frame[5], true);
        } else if (dispatch->frame.size() > 4) {
            noteControlSent(dispatch->frame[4], 0, false);
        }
    }
    if (dispatch->frame.size() > 4) {
        QString hex;
        for (std::size_t i = 4; i + 1 < dispatch->frame.size(); ++i) {
            hex += QStringLiteral("%1 ").arg(dispatch->frame[i], 2, 16, QLatin1Char('0'));
        }
        m_lastOutboundCiv = hex.trimmed();
        m_lastOutboundCivAtMs = nowMs;
    }
    m_session->sendCiv(dispatch->frame);
    serviceSchedulerWaiters(nowMs);
}

QVariantMap IcomCivBackend::schedulerDiagnostics() const
{
    const IcomCivScheduler::Stats stats = m_civScheduler.stats();
    QVariantMap out;
    out.insert(QStringLiteral("idle"), m_civScheduler.idle());
    out.insert(QStringLiteral("slotMs"), IcomCivScheduler::kSlotMs);
    out.insert(QStringLiteral("readTimeoutMs"), IcomCivScheduler::kReadTimeoutMs);
    out.insert(QStringLiteral("queueDepth"), static_cast<qulonglong>(stats.queueDepth));
    out.insert(QStringLiteral("readInFlight"), stats.readInFlight);
    out.insert(QStringLiteral("inFlightKey"), QString::fromStdString(stats.inFlightKey));
    out.insert(QStringLiteral("queued"), static_cast<qulonglong>(stats.queued));
    out.insert(QStringLiteral("dispatched"), static_cast<qulonglong>(stats.dispatched));
    out.insert(QStringLiteral("coalesced"), static_cast<qulonglong>(stats.coalesced));
    out.insert(QStringLiteral("replies"), static_cast<qulonglong>(stats.replies));
    out.insert(QStringLiteral("staleReplies"), static_cast<qulonglong>(stats.staleReplies));
    out.insert(QStringLiteral("timeouts"), static_cast<qulonglong>(stats.timeouts));
    out.insert(QStringLiteral("pendingPttIntent"), m_pendingPttIntent.has_value());
    if (m_pendingPttIntent) {
        out.insert(QStringLiteral("pttIntent"), *m_pendingPttIntent);
        // REMAINING, not a deadline. The backend's clock is monotonic since
        // construction, so the absolute value means nothing to a consumer;
        // "how much longer can this suppress a contradictory report" is the
        // question anyone reads this field to answer.
        out.insert(QStringLiteral("pttIntentRemainingMs"),
                   std::max<qint64>(0, m_pendingPttUntilMs - nowMs()));
    }
    return out;
}

void IcomCivBackend::serviceSchedulerWaiters(qint64 nowMs)
{
    // COLLECT, ERASE, THEN EMIT. extensionResult is a direct connection, so a
    // slot that registers another waiter would reallocate the vector under an
    // iterator we are still holding. Finishing all mutation first makes the
    // re-entrant case merely queue more work instead of corrupting the walk.
    std::vector<quint64> ready;
    for (auto it = m_schedulerWaiters.begin(); it != m_schedulerWaiters.end();) {
        if (!m_civScheduler.idle() && nowMs < it->deadlineMs) {
            ++it;
            continue;
        }
        ready.push_back(it->requestId);
        it = m_schedulerWaiters.erase(it);
    }
    if (ready.empty())
        return;
    QVariantMap result = schedulerDiagnostics();
    result.insert(QStringLiteral("timedOut"), !m_civScheduler.idle());
    for (quint64 requestId : ready)
        emit extensionResult(requestId, result);
}

void IcomCivBackend::sendUserCommand(const std::vector<std::uint8_t>& frame)
{
    if (!m_session || !m_connected)
        return;
    const qint64 now = nowMs();
    const std::string key = semanticKey(frame);
    const std::optional<CivFrame> parsed = parseFrame(frame);
    if (parsed) {
        noteControlScheduled(parsed->cmd, parsed->sub, parsed->hasSub);
    }
    const bool failSafeUnkey = parsed && parsed->cmd == cmd::kControl
        && parsed->hasSub && parsed->sub == control::kPtt
        && !parsed->data.empty() && parsed->data.front() == 0;
    queueWrite(frame, key, failSafeUnkey ? IcomCivScheduler::Priority::Emergency
                                        : IcomCivScheduler::Priority::Operator);
    if (const auto confirmation = confirmationFor(frame)) {
        // Let the radio apply the write before asking.  The confirmation has
        // the same semantic generation, while any read already on the wire is
        // older and will be rejected by observe().
        queueRead(*confirmation, key, IcomCivScheduler::Priority::Operator, now + 60);
    }
    pumpCiv(now);
}

void IcomCivBackend::setSliceFrequency(int, double hz)
{
    if (hz <= 0.0)
        return;
    sendUserCommand(cmdSetFrequency(m_session ? m_session->civAddress() : 0xA4,
                                    static_cast<std::uint64_t>(std::llround(hz))));
}

void IcomCivBackend::setSliceMode(int, const QString& mode)
{
    bool data = false;
    auto civ = modeFromNeutral(mode.toStdString(), data);
    if (!civ) {
        // No IC-705 equivalent (SAM, DRM, DSB). Refusing beats substituting USB:
        // a slice that asked for SAM and silently got USB has a mode indicator
        // that lies about what is being demodulated.
        //
        // But refusing SILENTLY leaves it lying too. SliceModel has already
        // taken the operator's choice by the time we see it, so a bare return
        // left the mode indicator reading SAM on a radio demodulating AM —
        // which is how a broadcast station ended up being received through a
        // 2.4 kHz window with the UI insisting it was in synchronous AM.
        // Re-assert what the radio is ACTUALLY in.
        //
        // QUEUED, for the same reason the refused pan centre is (see
        // setPanCenter). SliceModel::setMode has already written the refused
        // mode into its own field and calls us from modeChangeRequested — and
        // it emits modeChanged(mode) on the line AFTER that signal returns. A
        // direct emit here is applied and then immediately announced away: the
        // model ends up holding AM while the last modeChanged the UI saw said
        // SAM, so the indicator still lies. Deferring one event-loop turn puts
        // the correction after that announcement.
        const QString actual = QString::fromStdString(modeToNeutral(m_mode, m_dataMode));
        if (!actual.isEmpty()) {
            const auto [lo, hi] =
                passbandForModeAndFilter(currentLadderMode().toStdString(), m_filter);
            QMetaObject::invokeMethod(this, [this, actual, lo, hi] {
                SliceDelta d;
                d.mode = actual;
                d.filterLow  = lo;
                d.filterHigh = hi;
                emit sliceChanged(sliceId(), d);
            }, Qt::QueuedConnection);
        }
        return;
    }
    // ADOPT THE MODE NOW, not when the radio reports it back.
    //
    // capabilities() derives the filter LADDER from m_mode — FIL1 is 3.0 kHz in
    // SSB and 9 kHz in AM — and publishCapabilities() below reads it. Leaving
    // m_mode stale until the radio's own 0x04 report arrived meant the passband
    // (computed from the argument) was right while the filter BUTTONS still
    // offered the previous mode's widths, and if CI-V Transceive is off that
    // report never comes at all. The radio's report corrects this if it
    // disagrees, exactly as it does for the preamp.
    m_mode = *civ;
    // KEEP THE FILTER SLOT across a mode change. Hardcoding FIL1 here meant
    // every mode change jumped to the widest filter, so an operator working a
    // narrow CW filter lost it the moment they visited another mode and came
    // back.
    const std::uint8_t addr = m_session ? m_session->civAddress() : 0xA4;
    // MODE AND THE DATA FLAG IN ONE FRAME, because command 06 cannot carry the
    // flag at all.
    //
    // THE BUG THIS FIXES: DIGU and USB are the same mode byte. Sending 06 01
    // alone asked for plain USB, so an operator selecting DIGU on a radio
    // sitting in DATA OFF got a radio modulating from the MICROPHONE while
    // AetherSDR's indicator, passband and capabilities all said DIGU. Digital
    // transmit looked completely wired and produced no output — no error
    // anywhere, because nothing was wrong except which modulator the radio was
    // listening to.
    //
    // ONE FRAME, not an ordered pair. Writing the ordinary mode is what clears
    // DATA on the radio, so mode-then-DATA is a sequence whose correctness
    // depends on both frames landing and landing in order; 26 states all three
    // at once and the radio applies or refuses them as a unit.
    if (m_model->hasVfoModeCommand) {
        m_dataMode = data;
        sendUserCommand(cmdSetVfoMode(addr, *civ, data, m_filter));
    } else {
        // A radio we cannot characterise. 06 has existed on every Icom for
        // decades; 26 has not, and a mode change the radio answers NG to is a
        // mode change that silently does not happen. No DATA control here, which
        // is what an unknown radio had before this existed.
        m_dataMode = false;
        sendUserCommand(cmdSetMode(addr, *civ, m_filter));
    }
    // CONFIRM. Everything above is a request; only the radio's own answer is
    // state (Constitution II). sendUserCommand queues that confirmation read
    // itself, at Operator priority and one generation ahead of any poll already
    // on the wire, and it is what corrects the optimistic publish below if the
    // radio refused or altered the change — a mode with no DATA variant, a band
    // where the radio will not enter it. One extra frame on the operator's own
    // mode change, not a new poll.
    // PUBLISH THE PASSBAND NOW, from the mode we just commanded.
    //
    // Waiting for the radio to report the mode back is not good enough: the
    // report only arrives if CI-V Transceive is on, and even then it lands
    // milliseconds later. radiocert's passband-after-mode-change stage caught
    // exactly that — CW then DIGU left the window at the previous mode's width,
    // so a decoder in a wide mode saw a narrow slot. The radio owns its DSP and
    // sends no passband, so this is the only place it can come from.
    const QString publishedMode = currentNeutralMode();
    const auto [low, high] =
        passbandForModeAndFilter(publishedMode.toStdString(), m_filter);
    SliceDelta d;
    d.mode = publishedMode;
    d.filterLow  = low;
    d.filterHigh = high;
    emit sliceChanged(sliceId(), d);
    // The new mode's filter ladder is a different three widths — republish so
    // the filter buttons stop offering the previous mode's.
    publishCapabilities();
}

void IcomCivBackend::setSliceFilter(int, int lowHz, int highHz)
{
    // The radio has three fixed IF filters, not a continuous passband, so this
    // can only SNAP. What the radio actually took comes back on its own mode
    // report — we must not echo the requested width as if it were applied.
    const int width = std::abs(highHz - lowHz);
    // The LADDER mode, not the neutral one: the two differ in RTTY, where the
    // radio's own widths are 2.4k/500/250 — see currentLadderMode().
    const QString neutral = currentLadderMode();
    // MODE-AWARE. Snapping against the SSB thresholds whatever the mode put
    // every AM width on FIL1 and every CW width on FIL3 — three buttons and one
    // filter, in both directions.
    const int filter = filterForWidthHz(neutral.toStdString(), width);
    m_filter = filter;
    const std::uint8_t addr = m_session ? m_session->civAddress() : 0xA4;
    // THE FILTER BUTTON MUST NOT DROP THE RADIO OUT OF DATA. Command 06 carries
    // mode and slot with no DATA byte, and writing it is what clears DATA on
    // the radio — so a filter change sent as 06 took an operator running FT8 in
    // USB-D back to plain USB and their transmit audio back to the microphone,
    // from a button that says nothing about the mode. 26 restates DATA with the
    // new slot in the same frame.
    //
    // m_dataMode here is the RADIO's reported state, not a guess: it is read at
    // connect, re-read after every front-panel mode change, and confirmed after
    // every mode write, so this re-asserts what the radio said (Constitution
    // II) rather than pushing a client belief over it.
    if (m_model->hasVfoModeCommand)
        sendUserCommand(cmdSetVfoMode(addr, m_mode, m_dataMode, filter));
    else
        sendUserCommand(cmdSetMode(addr, m_mode, filter));
    // A write is intent; the radio's own reply is state. sendUserCommand
    // schedules that readback itself now (confirmationFor maps 26 -> read 26,
    // 06 -> read 04), so it also corrects the optimistic passband below if the
    // radio clamps or refuses the requested slot while CI-V Transceive is off.
    //
    // PUBLISH THE PASSBAND NOW, for the same reason setSliceMode does: the
    // radio's mode report only comes back if CI-V Transceive is on, and the
    // operator who just clicked a filter button is owed an immediate answer.
    // If the radio disagrees its own report corrects this a few ms later.
    SliceDelta d;
    const auto [low, high] = passbandForModeAndFilter(neutral.toStdString(), filter);
    d.filterLow  = low;
    d.filterHigh = high;
    emit sliceChanged(sliceId(), d);
}

void IcomCivBackend::setSliceAgc(int, const QString& mode, int)
{
    m_agcMode = mode;
    // thresholdDb has NOWHERE to go: the radio offers FAST/MID/SLOW and no
    // threshold. A documented no-op beats inventing a mapping.
    const QString m = mode.toUpper();
    int value = 2;   // MID
    if (m == QLatin1String("FAST"))
        value = 1;
    else if (m == QLatin1String("SLOW"))
        value = 3;
    else if (m == QLatin1String("OFF"))
        value = 1;   // the radio has no AGC-off; FAST is the closest honest thing
    sendUserCommand(cmdSetFunction(m_session ? m_session->civAddress() : 0xA4,
                                   func::kAgc, value));
}

void IcomCivBackend::setPanCenter(const QString&, double hz, PanCenterIntent intent)
{
    if (m_scopeSpanHz <= 0)
        return;

    const double centreMhz = static_cast<double>(m_scopeCentreHz) / 1e6;
    const double widthMhz  = static_cast<double>(m_scopeSpanHz * 2) / 1e6;

    // A ZOOM's centre is refused, and re-asserted immediately.
    //
    // Centre and bandwidth travel together on a range change, so every zoom
    // click arrives here carrying a centre. Honouring it would walk the VFO
    // across the band one click at a time, which is what this whole method used
    // to do to a DRAG as well. Without the re-assert the widget keeps its
    // optimistic centre for up to a frame and the trace visibly slides before
    // the next sweep contradicts it.
    //
    // QUEUED, and that is not incidental. RadioModel writes the REQUESTED
    // centre into the pan model on the line after it calls us, so a direct emit
    // here is overwritten by the very value we are refusing. Deferring to the
    // next event loop iteration puts the correction after that write and still
    // lands inside the same frame — sooner than the next sweep would.
    if (intent != PanCenterIntent::Drag) {
        qCDebug(lcIcomPan) << "pan-centre from a range change REFUSED;"
                           << "asked" << hz << "Hz, radio is at" << m_scopeCentreHz << "Hz";
        QMetaObject::invokeMethod(this, [this, centreMhz, widthMhz] {
            emit panCenterBandwidthChanged(panId(), centreMhz, widthMhz);
        }, Qt::QueuedConnection);
        return;
    }

    // A DRAG RETUNES, and on this radio there is no third option.
    //
    // In centre mode the scope window IS the operating frequency — the radio
    // offers no way to offset one from the other, and its FIXED mode is not a
    // free-form window either (three saved edge presets per band, 0x27 0x1E,
    // which following a drag would overwrite thirty times a second). So the
    // window cannot slide over stationary spectrum the way it does on a Flex:
    // the only way to show the operator the spectrum they dragged toward is to
    // tune there.
    //
    // This method used to refuse a drag too, and re-assert. The result was a
    // trace that slid under the mouse and snapped back a frame later, on every
    // attempt — the panadapter's most basic gesture reading as a bug.
    //
    // The DEAD ZONE is what keeps a click from being a tune. A press-and-release
    // with a pixel of hand movement arrives here as a centre a few Hz away, and
    // one-to-one tuning would move the dial on every stray click. One percent of
    // the visible span is far below what anyone can aim at and far above jitter.
    const double requestedHz = hz;
    const double deltaHz = requestedHz - static_cast<double>(m_scopeCentreHz);
    const double deadZoneHz = static_cast<double>(m_scopeSpanHz) * kPanDragDeadZoneFraction;
    if (std::abs(deltaHz) < deadZoneHz) {
        qCDebug(lcIcomPan) << "pan drag inside the dead zone (" << deltaHz << "Hz of"
                           << deadZoneHz << ") — ignored";
        return;
    }

    qCDebug(lcIcomPan) << "pan drag retunes:" << m_scopeCentreHz << "Hz ->"
                       << requestedHz << "Hz (delta" << deltaHz << ")";
    setSliceFrequency(sliceId(), requestedHz);
}

void IcomCivBackend::setPanBandwidth(const QString&, double hz)
{
    if (hz <= 0.0 || !m_model->hasScope)
        return;
    // hz is a TOTAL width and Icom's span is a HALF-width, so the conversion is
    // not a rename. It also SNAPS to one of eight values — what was actually
    // taken comes back with the next sweep, via panCenterBandwidthChanged.
    const int requested = spanForBandwidthHz(static_cast<int>(std::llround(hz)));
    int target = requested;

    // NEAREST IS NOT ENOUGH — see adjacentScopeSpanHz. A zoom step of 1.5
    // against spans spaced by 2 and 2.5 lands short of the midpoint every time
    // it widens, so nearest-snapping returned the current span and the command
    // was a no-op. Zoom out did nothing at all eight spans.
    //
    // When the request resolves back to where we already are, honour its
    // DIRECTION instead of its magnitude and move exactly one detent. Quantised
    // zoom is the truth about this radio; inert zoom is a bug.
    if (m_scopeSpanHz > 0 && target == m_scopeSpanHz) {
        const int wanted = static_cast<int>(std::llround(hz / 2.0));
        if (wanted < m_scopeSpanHz)
            target = adjacentScopeSpanHz(target, -1);
        else if (wanted > m_scopeSpanHz)
            target = adjacentScopeSpanHz(target, +1);
        else
            return;   // genuinely no change asked for
    }

    qCDebug(lcIcomPan) << "pan-bandwidth request" << hz << "Hz ->"
                       << "span" << target << "Hz (nearest was" << requested
                       << ", radio is at" << m_scopeSpanHz << ")";
    sendUserCommand(cmdScopeSpan(m_session ? m_session->civAddress() : 0xA4, target));
}

// The parameter is named gainDb by the seam and is a PERCENT here — see the
// unit suffix published at connect. Renaming it would mean renaming the seam,
// which is right for a Flex and wrong only for the radios that have no dB.
void IcomCivBackend::setPanRfGain(const QString&, int gainDb)
{
    m_rfGainPercent = std::clamp(gainDb, 0, 100);
    sendUserCommand(cmdSetLevel(m_session ? m_session->civAddress() : 0xA4,
                                level::kRf, percentToLevelRaw(std::clamp(gainDb, 0, 100))));
}

// ADOPT THE REQUESTED STEP, do not wait for an echo.
//
// A set on this radio is answered with a bare FB — an acknowledgement, not a
// report of the new value. Nothing follows it. Both of these used to publish
// nothing and leave the button to be corrected by a `panPreampChanged` that
// never arrives, so the control cycled OFF -> P.AMP1 and then stuck: the click
// emitted step 2, the widget reverted itself to its pre-click state waiting for
// the radio, and the radio said only "understood".
//
// The optimistic publish is what the connect-time and front-panel reads are for:
// if the radio refused the request — an IC-705 has no P.AMP2 above 50 MHz, and
// no attenuator there at all — the next unsolicited 16 02 / 11 report corrects
// it. Claiming a position the radio took is right far more often than showing
// none at all.
void IcomCivBackend::setPanPreamp(const QString&, int step)
{
    // Clamp, never refuse — the seam's rule for every stepped control.
    const int wanted = std::clamp(step, 0, 2);
    m_preampStep = wanted;
    sendUserCommand(cmdSetFunction(m_session ? m_session->civAddress() : 0xA4,
                                   func::kPreamp, wanted));
    emit panPreampChanged(panId(), wanted);
}

void IcomCivBackend::setPanAttenuator(const QString&, int step)
{
    // Step 1 is the 20 dB position; step 0 is off. The dB figure lives here
    // rather than in the label because the label is what the operator reads and
    // this is what the radio takes.
    // THE dB COMES FROM THE MODEL'S TABLE, not from a literal. A hardcoded 20
    // is the IC-705's single step; a radio with a different ladder would get
    // that number sent to a register that means something else.
    const auto steps = attenStepsFor(*m_model);
    if (steps.empty())
        return;   // no verified ladder — the control was never published either
    const int wanted =
        std::clamp(step, 0, static_cast<int>(steps.size()) - 1);
    m_attenStep = wanted;
    sendUserCommand(cmdSetAttenuator(m_session ? m_session->civAddress() : 0xA4,
                                     steps[static_cast<std::size_t>(wanted)].db));
    emit panAttenuatorChanged(panId(), wanted);
}

void IcomCivBackend::setSliceRxAntenna(int, const QString& antenna)
{
    if (!m_model || m_model->civAddress != 0xB6)
        return;
    const bool external = antenna.compare(QStringLiteral("RX-ANT"),
                                          Qt::CaseInsensitive) == 0;
    m_rxAntennaExternal = external;
    sendUserCommand(cmdSetRxAntenna(m_session ? m_session->civAddress() : 0xB6,
                                    external));
}

void IcomCivBackend::setSpeechProcessor(bool on, int level)
{
    m_compEnable = on;
    m_compLevelPercent = level;
    const std::uint8_t addr = m_session ? m_session->civAddress() : 0xA4;

    // TWO REGISTERS, not one. The operator's control is Flex-shaped — an enable
    // plus NOR/DX/DX+ — and on this radio the enable is a function (16 44) while
    // "how hard" is a level (14 0E, 0000..0255 spanning 0..10). Sending only the
    // enable is what left AetherSDR's PROC disagreeing with a front panel that
    // plainly showed the compressor on.
    sendUserCommand(cmdSetFunction(addr, func::kCompressor, on ? 1 : 0));
    if (!on)
        return;   // the level is meaningless while the compressor is bypassed

    // NOR / DX / DX+ onto the radio's 0..10 scale. Icom publishes no mapping —
    // these are thirds of its range, which is the honest reading of a
    // three-position control against a continuous one, and they are here rather
    // than open-coded so the choice is visible and adjustable.
    static constexpr std::array<int, 3> kProcLevels{3, 6, 9};   // of 10
    const int preset = std::clamp(level, 0, 2);
    const int raw = kProcLevels[static_cast<std::size_t>(preset)] * 255 / 10;
    sendUserCommand(cmdSetLevel(addr, level::kCompLevel, raw));
}

void IcomCivBackend::setMicGain(int gainPercent)
{
    m_micGainPercent = gainPercent;
    m_micGainReported = true;
    sendUserCommand(cmdSetLevel(m_session ? m_session->civAddress() : 0xA4,
                                level::kMicGain, percentToLevelRaw(gainPercent)));
}

void IcomCivBackend::setTxAudioMonitor(bool on)
{
    m_monitorOn = on;
    // The FUNCTION only. The radio has a separate monitor LEVEL (14 15) and no
    // seam verb carries it, so setting it here would either overwrite whatever
    // the operator dialled in on the radio or invent a value — both worse than
    // leaving their own setting alone and toggling what was actually asked for.
    sendUserCommand(cmdSetFunction(m_session ? m_session->civAddress() : 0xA4,
                                   func::kMonitorFn, on ? 1 : 0));
}

void IcomCivBackend::setTxMonitor(bool on, int levelPercent)
{
    const std::uint8_t addr = m_session ? m_session->civAddress() : 0xA4;
    m_monitorOn = on;
    m_monitorLevelPercent = std::clamp(levelPercent, 0, 100);
    if (m_monitorSent != (on ? 1 : 0)) {
        m_monitorSent = on ? 1 : 0;
        sendUserCommand(cmdSetFunction(addr, func::kMonitorFn, on ? 1 : 0));
    }
    sendUserCommand(cmdSetLevel(addr, level::kMonitor,
                                percentToLevelRaw(m_monitorLevelPercent)));
}

void IcomCivBackend::setSliceNoiseReduction(int, bool on, int level)
{
    m_nrLevelPercent = level;
    const std::uint8_t addr = m_session ? m_session->civAddress() : 0xA4;
    if (m_nrEnableSent != (on ? 1 : 0)) {
        m_nrEnableSent = on ? 1 : 0;
        sendUserCommand(cmdSetFunction(addr, func::kNoiseReduce, on ? 1 : 0));
    }
    // The level register survives the function being switched off, so pushing
    // it while disabled would silently change what the operator gets back when
    // they re-enable. Only touch it when it can take effect.
    if (on)
        sendUserCommand(cmdSetLevel(addr, level::kNrLevel, percentToLevelRaw(level)));
}

void IcomCivBackend::setSliceNoiseBlanker(int, bool on, int level)
{
    m_nbLevelPercent = level;
    const std::uint8_t addr = m_session ? m_session->civAddress() : 0xA4;
    if (m_nbEnableSent != (on ? 1 : 0)) {
        m_nbEnableSent = on ? 1 : 0;
        sendUserCommand(cmdSetFunction(addr, func::kNoiseBlanker, on ? 1 : 0));
    }
    if (on)
        sendUserCommand(cmdSetLevel(addr, level::kNbLevel, percentToLevelRaw(level)));
}

void IcomCivBackend::setSliceAutoNotch(int, bool on)
{
    if (m_anfEnableSent == (on ? 1 : 0))
        return;
    m_anfEnableSent = on ? 1 : 0;
    sendUserCommand(cmdSetFunction(m_session ? m_session->civAddress() : 0xA4,
                                   func::kAutoNotch, on ? 1 : 0));
}

void IcomCivBackend::setSliceManualNotch(int, bool on, int position)
{
    m_notchPosPercent = position;
    const std::uint8_t addr = m_session ? m_session->civAddress() : 0xA4;
    // Same enable-dedupe as NR and NB, and for the same reason documented on
    // m_nrEnableSent: the position setter carries the current enable with it, so
    // without this a drag would put 16 48 on the wire on every tick.
    if (m_mnEnableSent != (on ? 1 : 0)) {
        m_mnEnableSent = on ? 1 : 0;
        sendUserCommand(cmdSetFunction(addr, func::kManualNotch, on ? 1 : 0));
    }
    // POSITION IS PUSHED EVEN WHEN THE NOTCH IS OFF, which is the opposite of
    // what NR and NB do above — and deliberately so. Their level registers are
    // an amount of processing, and writing one while disabled changes what the
    // operator gets back on re-enable. This one is a PLACE: 14 0D is where the
    // notch will appear, the operator sets it by dragging a marker they can
    // see, and refusing the write would leave the marker and the notch in
    // different places until the next drag after enabling.
    sendUserCommand(cmdSetLevel(addr, level::kNotchPos, percentToLevelRaw(position)));
}

// AF GAIN. Read and decoded since the first bring-up, and until now never
// settable: `setSliceAudioGain` was simply not overridden, so the operator's AF
// slider moved, persisted, and reached no register. `controls map` reported it
// as decode-only, which is what made a dead slider distinguishable from a
// working one.
void IcomCivBackend::setSliceAudioGain(int, int gainPercent)
{
    m_afGainPercent = std::clamp(gainPercent, 0, 100);
    sendUserCommand(cmdSetLevel(m_session ? m_session->civAddress() : 0xA4,
                                level::kAf, percentToLevelRaw(m_afGainPercent)));
}

// VOX. The enable is a function (16 46) and the trigger threshold a level
// (14 16) — the same two-register shape the speech processor has, and the same
// reason both arrive together.
//
// THE DELAY IS NOT HERE. The guide puts VOX DELAY in the SET menu at
// 1A 05 0359 in 0.1 s steps, and 14 17 is the ANTI-vox gain, which is a third
// control again. Writing a delay we were handed in milliseconds into a menu
// item measured in tenths would be an invented conversion on a setting the
// operator may have deliberately chosen, so it is left alone and said so.
void IcomCivBackend::setVox(bool on, int level, int delayMs)
{
    Q_UNUSED(delayMs);
    const std::uint8_t addr = m_session ? m_session->civAddress() : 0xA4;
    m_voxOn = on;
    m_voxLevelPercent = level;
    if (m_voxEnableSent != (on ? 1 : 0)) {
        m_voxEnableSent = on ? 1 : 0;
        sendUserCommand(cmdSetFunction(addr, func::kVox, on ? 1 : 0));
    }
    // The level slider is an explicit operator intent even while VOX is off:
    // the register survives disable and determines the next enable threshold.
    sendUserCommand(cmdSetLevel(addr, level::kVoxGain, percentToLevelRaw(level)));
}

// THE ANTENNA TUNER, and it keys.
//
// `1C 01 02` starts a matching cycle on an EXTERNAL AH-705; `1C 01 00` bypasses.
// There is no command to ask whether a tuner is attached, so a start on a radio
// with none is a request that simply does nothing — which is why
// capabilities().hasTuner stays operator-driven rather than claiming knowledge
// the protocol cannot give us.
void IcomCivBackend::setAtu(bool start)
{
    sendUserCommand(cmdSetTuner(m_session ? m_session->civAddress() : 0xA4,
                                start ? 0x02 : 0x00));
    // sendUserCommand queues a readback after the radio has applied the write;
    // that confirmation is also what lets the transient tuning state settle.
}

void IcomCivBackend::setSliceSquelch(int, bool on, int level)
{
    m_squelchPercent = on ? level : 0;
    // NO SQUELCH ENABLE EXISTS on this radio — the threshold IS the control,
    // and squelch is "off" when it sits at zero. Mapping the UI's toggle onto
    // the threshold is the only honest translation available; the alternative
    // is a switch that does nothing.
    sendUserCommand(cmdSetLevel(m_session ? m_session->civAddress() : 0xA4,
                                level::kSquelch, on ? percentToLevelRaw(level) : 0));
}

void IcomCivBackend::setRitEnabled(bool on)
{
    m_ritOn = on;
    sendUserCommand(cmdRitEnable(m_session ? m_session->civAddress() : 0xA4, on));
}

void IcomCivBackend::setXitEnabled(bool on)
{
    m_xitOn = on;
    sendUserCommand(cmdXitEnable(m_session ? m_session->civAddress() : 0xA4, on));
}

void IcomCivBackend::setRitOffset(int hz)
{
    m_ritOffsetHz = hz;
    // ONE offset register serves both RIT and XIT on this radio — 21 00 is the
    // shift, and 21 01 / 21 02 decide which of receive and transmit it applies
    // to. A caller that expects two independent offsets will not get them.
    sendUserCommand(cmdTuneOffsetHz(m_session ? m_session->civAddress() : 0xA4, hz));
}

void IcomCivBackend::setKeying(bool key)
{
    if (!m_model->hasTransmit)
        return;   // an unknown radio is not advertised as transmit-capable
    m_pendingPttIntent = key;
    m_pendingPttUntilMs = nowMs() + 1000;
    sendUserCommand(cmdSetPtt(m_session ? m_session->civAddress() : 0xA4, key));
    // PUBLISH IT. Setting m_keyed silently here and leaving the announcement to
    // the poll does not work now that the poll only speaks on change: our own
    // keying moved the variable, so the poll's answer matched it and nothing
    // was ever emitted. The model then read mox=false through an entire live
    // transmission — with the radio plainly on the air and its own meters
    // moving — which silently mis-gates everything downstream that asks
    // "are we transmitting".
    if (m_keyed != key) {
        m_keyed = key;
        TransmitDelta t;
        t.mox = key;
        emit transmitChanged(t);
    }
    m_meters.setTransmitting(key);
    if (!key && m_session)
        m_session->flushTxAudio();   // queued audio belongs to the transmission that ended
}

void IcomCivBackend::setTune(bool on, int tunePowerPercent)
{
    // THERE IS NO TUNE-CARRIER COMMAND. `1C 01` is the antenna tuner, which is
    // a different feature and may not even be attached. A steady tune carrier
    // is COMPOSED: set the drive, then key. The mode save/restore that a full
    // implementation needs is deliberately absent here rather than half-done —
    // see the design note.
    if (on) {
        if (!m_tuning)
            m_preTuneTxPowerPercent = m_txPowerPercent;
        if (tunePowerPercent >= 0)
            setTxPower(tunePowerPercent);
        // Raise the tone BEFORE keying, so no part of the keyed window is
        // silent — a tuner sampling that edge can otherwise read infinite SWR.
        m_tuning = true;
        m_tunePhase = 0.0;
        setKeying(true);
        return;
    }

    // Unkey BEFORE restoring ordinary RF power. The tune setpoint is temporary
    // and must not become the radio's new operating drive after the carrier.
    setKeying(false);
    m_tuning = false;
    if (m_preTuneTxPowerPercent >= 0) {
        const int restore = m_preTuneTxPowerPercent;
        m_preTuneTxPowerPercent = -1;
        setTxPower(restore);
    }
}

void IcomCivBackend::setTxPower(int percent)
{
    m_txPowerPercent = std::clamp(percent, 0, 100);
    sendUserCommand(cmdSetLevel(m_session ? m_session->civAddress() : 0xA4,
                                level::kRfPower, percentToLevelRaw(m_txPowerPercent)));
}

// EVERY registry row a frame belongs to, not the first.
//
// One CI-V frame can carry more than one operator control: 0x06 sets the mode
// AND the filter slot in the same message, and both are real controls with their
// own seam verbs. Returning the first match credited `mode` and left `filter`
// looking unwired on a radio where they cannot be separated.
static void forEachSpecForFrame(std::uint8_t cmd, std::uint8_t sub, bool hasSub,
                                const std::function<void(const icom::ControlSpec&)>& fn)
{
    // The SET address is the row's identity, but a radio answers a read with its
    // own command and reports a change with a third. Without this a control that
    // is read at connect and reported unsolicited — which is most of the tuning
    // plane — never registered as seen.
    std::uint8_t setCmd = cmd;
    switch (cmd) {
    case cmd::kReadFreq:    case cmd::kSetFreqTrx: setCmd = cmd::kSetFreq; break;
    case cmd::kReadMode:    case cmd::kSetModeTrx: setCmd = cmd::kSetMode; break;
    default: break;
    }

    // A 26 frame states MODE, DATA AND FILTER, so it satisfies three rows, not
    // one. Without this the mode and filter rows read as never-sent the moment
    // writes moved onto 26 — `controls.map` would report two controls
    // unexercised on a session that had just exercised both.
    const bool alsoModeRows = cmd == cmd::kVfoMode && hasSub && sub == vfoMode::kSelected;

    for (const auto& c : icom::controlSpecs()) {
        if (c.cmd != setCmd && !(alsoModeRows && c.cmd == cmd::kSetMode))
            continue;
        if (c.hasSub && (!hasSub || c.sub != sub))
            continue;
        fn(c);
    }
}

void IcomCivBackend::noteControlSent(std::uint8_t cmd, std::uint8_t sub, bool hasSub)
{
    forEachSpecForFrame(cmd, sub, hasSub, [this](const icom::ControlSpec& c) {
        const QString id = QString::fromUtf8(c.id.data(), static_cast<int>(c.id.size()));
        m_controlsSent.insert(id);
        // We commanded it, so the mirror holds a real value from here on.
        m_controlsValueKnown.insert(id);
    });
}

void IcomCivBackend::noteControlScheduled(std::uint8_t cmd, std::uint8_t sub,
                                          bool hasSub)
{
    forEachSpecForFrame(cmd, sub, hasSub, [this](const icom::ControlSpec& c) {
        const QString id = QString::fromUtf8(c.id.data(), static_cast<int>(c.id.size()));
        m_controlsScheduled.insert(id);
    });
}

void IcomCivBackend::noteControlSeen(std::uint8_t cmd, std::uint8_t sub, bool hasSub)
{
    forEachSpecForFrame(cmd, sub, hasSub, [this](const icom::ControlSpec& c) {
        const QString id = QString::fromUtf8(c.id.data(), static_cast<int>(c.id.size()));
        m_controlsSeen.insert(id);
        // The radio answered for this row, so the decode above adopted its
        // value into the scrub mirror. This is the OTHER half of "we know what
        // this control is set to" — the half that does not require the operator
        // to have touched it. Only sendCiv-issued connect reads reach here;
        // they are the reads whose answers populate the mirrors.
        if (c.wiring != icom::Wiring::SendOnly)
            m_controlsValueKnown.insert(id);
    });
}

QVariantList IcomCivBackend::controlMap() const
{
    const auto sv = [](std::string_view v) {
        return QString::fromUtf8(v.data(), static_cast<int>(v.size()));
    };

    QVariantList out;
    // A DIAGNOSTIC ROW FIRST. Without it an all-false `seenThisSession` column
    // is ambiguous: it looks the same whether the radio is silent, the registry
    // matches nothing, or the observation hook is not running at all. The two
    // counters separate those three.
    {
        QVariantMap diag;
        diag.insert(QStringLiteral("id"), QStringLiteral("_diagnostics"));
        diag.insert(QStringLiteral("framesObserved"), static_cast<qint64>(m_framesObserved));
        diag.insert(QStringLiteral("controlsSeen"), m_controlsSeen.size());
        diag.insert(QStringLiteral("controlsSent"), m_controlsSent.size());
        out.append(diag);
    }
    for (const auto& c : icom::controlSpecs()) {
        const QString id = sv(c.id);
        QVariantMap m;
        m.insert(QStringLiteral("id"), id);
        m.insert(QStringLiteral("label"), sv(c.label));
        m.insert(QStringLiteral("civ"),
                 c.hasSub ? QStringLiteral("%1 %2")
                                .arg(c.cmd, 2, 16, QLatin1Char('0'))
                                .arg(c.sub, 2, 16, QLatin1Char('0'))
                          : QStringLiteral("%1").arg(c.cmd, 2, 16, QLatin1Char('0')));
        m.insert(QStringLiteral("plane"), sv(icom::planeName(c.plane)));
        m.insert(QStringLiteral("encoding"), sv(icom::encodingName(c.encoding)));
        m.insert(QStringLiteral("wiring"), sv(icom::wiringName(c.wiring)));
        m.insert(QStringLiteral("rawRange"),
                 QStringLiteral("%1..%2").arg(c.rawLow).arg(c.rawHigh));
        m.insert(QStringLiteral("neutralRange"),
                 c.neutralUnit.empty()
                     ? QString()
                     : QStringLiteral("%1..%2 %3").arg(c.neutralLow).arg(c.neutralHigh)
                           .arg(sv(c.neutralUnit)));
        m.insert(QStringLiteral("seamVerb"), sv(c.seamVerb));
        m.insert(QStringLiteral("uiTarget"), sv(c.uiTarget));
        m.insert(QStringLiteral("readAtConnect"), c.readAtConnect);
        if (!c.note.empty())
            m.insert(QStringLiteral("note"), sv(c.note));

        // OBSERVED, next to declared. The table says what the code intends; these
        // two say what this session has actually put on the wire and taken off
        // it. A row claiming `both` with sent=false and seen=false after a full
        // connect is the interesting case.
        m.insert(QStringLiteral("sentThisSession"), m_controlsSent.contains(id));
        m.insert(QStringLiteral("seenThisSession"), m_controlsSeen.contains(id));

        // The gap, named. Anything other than an empty string here is a finding
        // rather than a description, which is what lets a caller sort by it.
        QString gap;
        if (c.wiring == icom::Wiring::Declared)
            gap = QStringLiteral("no code path at all — the constant exists and nothing uses it");
        else if (c.wiring == icom::Wiring::DecodeOnly && c.seamVerb.empty())
            gap = QStringLiteral("readable but not settable — no seam verb reaches this register");
        else if (c.wiring == icom::Wiring::SendOnly)
            gap = QStringLiteral("settable but never read back — the control opens at our default, not the radio's");
        else if (!c.uiTarget.empty() && c.wiring == icom::Wiring::DecodeOnly)
            gap = QStringLiteral("the UI control exists and reaches no register");
        m.insert(QStringLiteral("gap"), gap);
        out.append(m);
    }
    return out;
}

// The METER half of the registry: every 0x15 subcommand this backend polls,
// with the scale it publishes and — the part that matters — how long ago it last
// produced a reading.
//
// AGE IS THE FINDING. A meter that is defined and never fed renders as a real
// instrument reading a quiet band, which is worse than a missing one
// (docs/radio-certification.md opens on exactly this). A definition alone proves
// nothing; `ageMs` is what separates a meter that works from one that merely
// exists. A TX-only meter reading -1 while receiving is correct and is labelled
// as such, so the two cannot be confused.
QVariantList IcomCivBackend::meterMap() const
{
    const auto sv = [](std::string_view v) {
        return QString::fromUtf8(v.data(), static_cast<int>(v.size()));
    };
    const qint64 now = nowMs();

    QVariantList out;
    for (const auto& m : meterSpecs()) {
        QVariantMap r;
        r.insert(QStringLiteral("id"), QStringLiteral("%1:%2").arg(sv(m.source), sv(m.name)));
        r.insert(QStringLiteral("civ"),
                 QStringLiteral("15 %1").arg(m.sub, 2, 16, QLatin1Char('0')));
        r.insert(QStringLiteral("unit"), sv(m.unit));
        r.insert(QStringLiteral("range"), QStringLiteral("%1..%2").arg(m.low).arg(m.high));
        r.insert(QStringLiteral("pollMs"), m.intervalMs);
        r.insert(QStringLiteral("when"),
                 m.when == MeterWhen::RxOnly   ? QStringLiteral("rx-only")
                 : m.when == MeterWhen::TxOnly ? QStringLiteral("tx-only")
                                               : QStringLiteral("always"));
        r.insert(QStringLiteral("visible"), m_meters.isVisible(m.id));

        const qint64 at = m_meters.lastReadingAtMs(m.id);
        const qint64 age = at > 0 ? now - at : -1;
        r.insert(QStringLiteral("ageMs"), age);
        r.insert(QStringLiteral("status"),
                 age < 0
                     ? (m.when == MeterWhen::TxOnly
                            ? QStringLiteral("IDLE — transmit-only, correct while receiving")
                            : QStringLiteral("NEVER FED — defined and no reading has ever arrived"))
                 : age > 5 * m.intervalMs
                     ? QStringLiteral("STALE — last reading is far older than its own poll interval")
                     : QStringLiteral("LIVE"));
        out.append(r);
    }
    return out;
}

QVariantMap IcomCivBackend::controlScrub(const QString& filter)
{
    const auto sv = [](std::string_view v) {
        return QString::fromUtf8(v.data(), static_cast<int>(v.size()));
    };

    QVariantMap out;
    if (!m_session || !m_connected) {
        out.insert(QStringLiteral("error"), QStringLiteral("not connected"));
        return out;
    }

    // NEVER THESE. Two of them transmit and the third powers the radio off over
    // a link that cannot power it back on. A scrub that has to be supervised is
    // a scrub nobody runs.
    static const QSet<QString> kNeverScrub = {
        QStringLiteral("ptt"), QStringLiteral("tuner"), QStringLiteral("power"),
    };

    QVariantList rows;
    int checked = 0, reached = 0, skipped = 0;
    for (const auto& c : icom::controlSpecs()) {
        const QString id = sv(c.id);
        if (kNeverScrub.contains(id))
            continue;
        if (!filter.isEmpty() && id != filter
            && sv(icom::planeName(c.plane)) != filter)
            continue;
        // Only rows we CLAIM to send. A declared-only or decode-only row has
        // nothing to drive, and reporting it as failed would confuse a missing
        // implementation with a broken one — the map already names those.
        if (c.wiring != icom::Wiring::Both && c.wiring != icom::Wiring::SendOnly)
            continue;
        if (c.seamVerb.empty())
            continue;

        ++checked;
        m_controlsSent.remove(id);
        m_controlsScheduled.remove(id);

        // DRIVE IT THROUGH THE SEAM, with a value that changes nothing.
        //
        // Re-asserting the current value is the whole trick: the question is
        // "does this intent reach the wire", not "does the radio obey", and a
        // scrub that moved every control would leave the operator's radio
        // rearranged.
        const bool driven = scrubDrive(c);
        const bool onWire = m_controlsSent.contains(id);
        const bool scheduled = m_controlsScheduled.contains(id);
        const bool linked = onWire || scheduled;
        if (linked)
            ++reached;
        else if (!driven)
            ++skipped;

        QVariantMap r;
        r.insert(QStringLiteral("id"), id);
        r.insert(QStringLiteral("civ"),
                 c.hasSub ? QStringLiteral("%1 %2")
                                .arg(c.cmd, 2, 16, QLatin1Char('0'))
                                .arg(c.sub, 2, 16, QLatin1Char('0'))
                          : QStringLiteral("%1").arg(c.cmd, 2, 16, QLatin1Char('0')));
        r.insert(QStringLiteral("seamVerb"), sv(c.seamVerb));
        r.insert(QStringLiteral("reachedWire"), onWire);
        r.insert(QStringLiteral("reachedScheduler"), scheduled);
        r.insert(QStringLiteral("status"),
                 linked    ? QStringLiteral("LINKED")
                 : !driven ? QStringLiteral("NOT-TESTED")
                           : QStringLiteral("BROKEN"));
        r.insert(QStringLiteral("verdict"),
                 onWire
                     ? QStringLiteral("the seam verb put this command on the wire")
                 : scheduled
                     ? QStringLiteral("the seam verb admitted this exact command to the CI-V "
                                      "scheduler; wait for `civ scheduler` idle with no new "
                                      "timeout to prove dispatch and readback")
                 : !driven
                     ? QStringLiteral("no safe way to re-assert this without changing "
                                      "the operator's setting — not a fault, not a pass")
                     : QStringLiteral("the seam verb ran and emitted NO frame — the "
                                      "intent reaches nothing"));
        rows.append(r);
    }

    out.insert(QStringLiteral("checked"), checked);
    out.insert(QStringLiteral("linked"), reached);
    out.insert(QStringLiteral("notTested"), skipped);
    out.insert(QStringLiteral("broken"), checked - reached - skipped);
    out.insert(QStringLiteral("rows"), rows);
    out.insert(QStringLiteral("note"),
               QStringLiteral("Each control is re-asserted at its CURRENT value, so nothing "
                              "on the radio moves. PTT, the antenna tuner and power-off are "
                              "never scrubbed. Scheduler admission is reported separately "
                              "from physical dispatch; finish the proof with `civ scheduler`."));
    return out;
}

// Re-assert one control at whatever it is already set to.
//
// Returns false when there is no safe way to drive this row — no tracked value,
// or a guard that would need the operator's setting changed to get past. That is
// a THIRD outcome, distinct from "the frame reached the radio" and from "the
// verb ran and emitted nothing", and collapsing it into either would misreport a
// control the scrub simply did not test.
//
// THE DEDUPE SENTINELS ARE CLEARED FIRST. NR, NB and both notches suppress an
// enable that matches what was last sent — correct in normal use, and fatal to a
// linkage check, because re-asserting the current value is precisely what the
// dedupe exists to swallow. Clearing the sentinel makes the verb send the SAME
// value it would have sent anyway, so nothing on the radio changes and the frame
// becomes observable.
bool IcomCivBackend::scrubDrive(const icom::ControlSpec& c)
{
    const int slice = sliceId();
    const QString pan = panId();
    const QString id = QString::fromUtf8(c.id.data(), static_cast<int>(c.id.size()));

    // A MIRROR NOBODY HAS ESTABLISHED IS NOT A CURRENT VALUE.
    //
    // Generalises the rule the nr/nb/anf/notch sentinels state one control at a
    // time. Until either the radio has answered for this row or we have
    // commanded it, the mirror holds a construction default — 0 % for every
    // gain, "off" for every switch — and re-asserting it is not a no-op, it is
    // a silent write of that default. A scrub documented as leaving the radio
    // untouched would deafen the receiver and report the row LINKED, because
    // the intent did reach the wire. NOT-TESTED is the honest outcome and the
    // scrub already has that state; the connect-time read burst establishes
    // every row here in the normal case, so this only fires when a read was
    // lost — which on the lossy link this backend exists for is one datagram.
    if (!m_controlsValueKnown.contains(id))
        return false;

    if (id == QLatin1String("rf.gain"))  { setPanRfGain(pan, m_rfGainPercent); return true; }
    if (id == QLatin1String("preamp"))   { setPanPreamp(pan, m_preampStep); return true; }
    if (id == QLatin1String("atten"))    { setPanAttenuator(pan, m_attenStep); return true; }
    if (id == QLatin1String("rx.antenna")) {
        setSliceRxAntenna(slice, m_rxAntennaExternal
                                   ? QStringLiteral("RX-ANT")
                                   : QStringLiteral("ANT1"));
        return true;
    }
    if (id == QLatin1String("squelch"))  { setSliceSquelch(slice, m_squelchPercent > 0, m_squelchPercent); return true; }
    if (id == QLatin1String("agc"))      { setSliceAgc(slice, m_agcMode, 0); return true; }
    if (id == QLatin1String("tx.power")) { setTxPower(m_txPowerPercent); return true; }
    if (id == QLatin1String("mic.gain")) { setMicGain(m_micGainPercent); return true; }
    if (id == QLatin1String("mod.input.dataoff")) {
        // Re-assert the CURRENT selection, which is the whole scrub contract:
        // the question is whether the intent reaches the wire, not whether the
        // radio obeys. Falls through to NOT-TESTED on a model with no verified
        // SET-menu map, or before the readback has landed — there is no safe
        // value to send in either case, and inventing one would move the
        // operator's radio.
        const auto mod = modulationProfileFor(*m_model);
        if (!mod || m_dataOffModInput < 0)
            return false;
        sendUserCommand(cmdWriteSetting(
            m_session ? m_session->civAddress() : m_model->civAddress,
            mod->dataOffInputItem,
            static_cast<std::uint8_t>(m_dataOffModInput)));
        return true;
    }
    if (id == QLatin1String("monitor") || id == QLatin1String("monitor.level")) {
        m_monitorSent = -1;
        setTxMonitor(m_monitorOn, m_monitorLevelPercent);
        return true;
    }
    if (id == QLatin1String("af.gain"))  { setSliceAudioGain(slice, m_afGainPercent); return true; }

    if (id == QLatin1String("vox") || id == QLatin1String("vox.gain")) {
        m_voxEnableSent = -1;   // defeat the dedupe; the value is unchanged
        setVox(m_voxOn, m_voxLevelPercent, 0);
        return true;
    }
    if (id == QLatin1String("rit.enable")) { setRitEnabled(m_ritOn); return true; }
    if (id == QLatin1String("xit.enable")) { setXitEnabled(m_xitOn); return true; }
    if (id == QLatin1String("rit.offset")) { setRitOffset(m_ritOffsetHz); return true; }

    if (id == QLatin1String("nr") || id == QLatin1String("nr.level")) {
        // UNKNOWN IS NOT OFF, and this is the guard that says so.
        //
        // -1 means the connect-time read never came back — which on the lossy
        // link this backend exists for is one lost datagram. Treating it as off
        // makes the scrub SEND "off": an operator with NR running has it
        // switched off by a check documented to leave the radio untouched, and
        // the row is reported LINKED because the intent did reach the wire.
        // NOT-TESTED is the honest answer, and the scrub already has that state.
        if (m_nrEnableSent < 0)
            return false;
        // The LEVEL is only sent while the function is on — the register
        // survives the function being switched off, so pushing it while
        // disabled would change what the operator gets back on re-enable.
        if (id.endsWith(QLatin1String(".level")) && m_nrEnableSent != 1)
            return false;
        // CAPTURE THE STATE BEFORE CLEARING THE SENTINEL. Reading it after the
        // assignment yields -1, which is not 1, so the scrub asked for NR OFF —
        // a read-only diagnostic that switched off the operator's noise
        // reduction and then reported the row LINKED, because the intent did
        // reach the wire. The three branches below get this right.
        const bool on = m_nrEnableSent == 1;
        m_nrEnableSent = -1;
        setSliceNoiseReduction(slice, on, m_nrLevelPercent);
        return true;
    }
    if (id == QLatin1String("nb") || id == QLatin1String("nb.level")) {
        // Unknown is not off — see the nr branch above.
        if (m_nbEnableSent < 0)
            return false;
        if (id.endsWith(QLatin1String(".level")) && m_nbEnableSent != 1)
            return false;
        const bool on = m_nbEnableSent == 1;
        m_nbEnableSent = -1;
        setSliceNoiseBlanker(slice, on, m_nbLevelPercent);
        return true;
    }
    if (id == QLatin1String("anf")) {
        // Unknown is not off — see the nr branch above.
        if (m_anfEnableSent < 0)
            return false;
        const bool on = m_anfEnableSent == 1;
        m_anfEnableSent = -1;
        setSliceAutoNotch(slice, on);
        return true;
    }
    if (id == QLatin1String("notch") || id == QLatin1String("notch.pos")) {
        // Unknown is not off — see the nr branch above.
        if (m_mnEnableSent < 0)
            return false;
        const bool on = m_mnEnableSent == 1;
        m_mnEnableSent = -1;
        setSliceManualNotch(slice, on, m_notchPosPercent);
        return true;
    }
    if (id == QLatin1String("comp") || id == QLatin1String("comp.level")) {
        // Same shape: 14 0E only goes out while the compressor is enabled.
        if (id.endsWith(QLatin1String(".level")) && !m_compEnable)
            return false;
        setSpeechProcessor(m_compEnable, m_compLevelPercent);
        return true;
    }

    if (id == QLatin1String("freq")) {
        // The DECODED frequency, not our last intent: this is read at connect,
        // so it is populated even in a session where nothing has tuned yet.
        if (m_frequencyHz <= 0)
            return false;
        setSliceFrequency(slice, static_cast<double>(m_frequencyHz));
        return true;
    }
    // No verified 0x26 means there is no frame that can carry data.mode. Report
    // NOT-TESTED, not MISSING after driving an unrelated bare 0x06 mode write.
    if (id == QLatin1String("data.mode") && !m_model->hasVfoModeCommand)
        return false;
    if (id == QLatin1String("mode") || id == QLatin1String("filter")
        || id == QLatin1String("data.mode")) {
        // data.mode rides the same verb: setSliceMode states mode, DATA and
        // slot in one 26 frame, so re-asserting the mode re-asserts the DATA
        // flag with it and the scrub sees it go out. The round-trip guard below
        // is what makes that safe — m_dataMode is now the RADIO's reported
        // state, so the re-assertion carries what the radio said rather than a
        // client guess.
        const QString m = currentNeutralMode();
        if (m.isEmpty())
            return false;
        // ONLY IF THE NAME ROUND-TRIPS. The neutral vocabulary is smaller than
        // the radio's: RTTY and RTTY-R both come back as DIGL/DIGU, so
        // re-asserting the neutral name on a radio in RTTY would command it to
        // LSB-D — a scrub documented as leaving the radio untouched changing
        // the operating mode. Where the round trip is lossy there is no way to
        // re-assert what the radio is in, which is what NOT-TESTED means.
        bool data = false;
        const auto civ = modeFromNeutral(m.toStdString(), data);
        if (!civ || *civ != m_mode || data != m_dataMode)
            return false;
        setSliceMode(slice, m);
        return true;
    }

    // scope.span short-circuits a request for the span it is already on, and
    // getting past that would mean actually zooming the operator's display.
    // rit.*, scope.onoff/output/reference track no current value, so
    // re-asserting one would invent it.
    return false;
}

void IcomCivBackend::traceCiv(bool outbound, std::span<const std::uint8_t> frame,
                              bool routine)
{
    QString hex;
    hex.reserve(static_cast<int>(frame.size()) * 3);
    for (std::uint8_t b : frame) {
        if (!hex.isEmpty())
            hex += QLatin1Char(' ');
        hex += QStringLiteral("%1").arg(b, 2, 16, QLatin1Char('0'));
    }
    m_civTrace.push_back({nowMs(), outbound, routine, hex});
    while (m_civTrace.size() > kCivTraceMax)
        m_civTrace.pop_front();

    // Also to the log, which outlives the backend. Decode the command and
    // subcommand alongside the raw bytes: `1a 06` means nothing to a reader
    // scanning a log, and the whole point of switching this on is to answer
    // "did the 1A 06 query go out, and did the radio answer it".
    if (lcIcomCiv().isDebugEnabled()) {
        // THE TWO CALL SITES PASS DIFFERENT LAYOUTS, so the command index is a
        // parameter and not an assumption:
        //
        //   TX (sendUserCommand) — the raw wire frame from buildFrame:
        //       FE FE <to> <from> <cmd> [<sub>] <data…> FD   -> cmd at 4
        //   RX (onCivFrame)      — re-serialised, envelope deliberately dropped
        //       <cmd> [<sub>] <data…>                        -> cmd at 0
        //
        // Reading index 4 for both printed a payload byte as the command on
        // every received frame, and silently printed NOTHING for any RX frame
        // shorter than five bytes — which is most of them. `1a 06 01 01`, the
        // reply this whole category was added to make visible, is four bytes
        // and came out undecorated. Exactly the wrong-but-plausible output the
        // comment below warns about, in the direction that was not checked.
        const int cmdIdx = outbound ? 4 : 0;
        QString tag;
        if (frame.size() > static_cast<std::size_t>(cmdIdx)) {
            const std::uint8_t c = frame[cmdIdx];
            tag = QStringLiteral(" cmd=%1").arg(c, 2, 16, QLatin1Char('0'));
            // Which commands carry a subcommand is a per-command fact, and
            // commandHasSubcommand() is the single list parseFrame() decodes
            // by. Keeping a second copy here would let the two drift, and a
            // drift would label command 0x05's first frequency digit as a
            // subcommand — the wrong-but-plausible output this tag exists to
            // avoid.
            if (frame.size() > static_cast<std::size_t>(cmdIdx) + 1
                && commandHasSubcommand(c)) {
                tag += QStringLiteral(" sub=%1")
                           .arg(frame[cmdIdx + 1], 2, 16, QLatin1Char('0'));
            }
        }
        qCDebug(lcIcomCiv).noquote().nospace()
            << (outbound ? "TX -> " : "RX <- ") << hex << tag;
    }
}

QVariantList IcomCivBackend::civTrace(bool includeRoutine) const
{
    const std::int64_t now = nowMs();
    QVariantList out;
    for (const auto& e : m_civTrace) {
        // ROUTINE POLL TRAFFIC IS HIDDEN BY DEFAULT, and this was learned by
        // using the tool: the very first real trace buried the one frame that
        // mattered under ~12 meter replies per second. The scope sweeps were
        // already excluded for the same reason; these are the rest of the
        // heartbeat — 15 xx meter answers and the 1C 00 transmit-state poll.
        //
        // Hidden, not dropped: `civ trace all` still returns them, because
        // "the meters stopped answering" is itself a diagnosis and needs them.
        if (!includeRoutine && e.routine) {
            continue;
        }
        QVariantMap m;
        // AGE, not a wall clock. The consumer is an agent correlating a reply
        // with a command it just sent, and "12 ms ago" answers that directly.
        m.insert(QStringLiteral("ageMs"), static_cast<qint64>(now - e.atMs));
        m.insert(QStringLiteral("dir"), e.outbound ? QStringLiteral("tx")
                                                   : QStringLiteral("rx"));
        m.insert(QStringLiteral("hex"), e.hex);
        out.append(m);
    }
    return out;
}

namespace {
// "27 15 00" / "271500" / "0x27,0x15" all parse. Deliberately permissive about
// separators and strict about everything else: a malformed byte is refused
// rather than silently dropped, because a short frame is still a legal frame
// and the radio would act on it.
std::optional<std::vector<std::uint8_t>> parseHexBytes(const QString& in)
{
    QString compact;
    for (QChar c : in) {
        if (c.isLetterOrNumber())
            compact += c;
        else if (c == QLatin1Char(' ') || c == QLatin1Char(',') || c == QLatin1Char(':'))
            continue;
        else
            return std::nullopt;
    }
    // Strip any "0x" pairs. NOTE this removes EVERY occurrence, not only
    // leading ones — "270x15" compacts the same way "0x27 0x15" does. Harmless,
    // because anything it would mangle was not valid hex to begin with, but the
    // filter is not the thing making that safe: isLetterOrNumber() above admits
    // 'g'-'z' and non-ASCII digits, and it is toUInt(&ok, 16) below that
    // rejects them. Correctness here is downstream, deliberately, rather than
    // in the character filter.
    compact.remove(QLatin1String("0x"), Qt::CaseInsensitive);
    if (compact.isEmpty() || compact.size() % 2 != 0)
        return std::nullopt;
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(compact.size() / 2));
    for (int i = 0; i < compact.size(); i += 2) {
        bool ok = false;
        const uint v = compact.mid(i, 2).toUInt(&ok, 16);
        if (!ok)
            return std::nullopt;
        out.push_back(static_cast<std::uint8_t>(v));
    }
    return out;
}
}  // namespace

void IcomCivBackend::invokeExtension(const QString& ns, const QString& verb, quint64 requestId,
                                     const QVariant& arg)
{
    if (ns != QLatin1String("icom")) {
        emit extensionError(requestId, QStringLiteral("unknown namespace %1").arg(ns));
        return;
    }
    if (verb == QLatin1String("tuner.start")) {
        // The ATU cycle — explicitly NOT setTune(). Exposed as an extension so
        // an operator with an AH-705 can reach it without the TUNE button
        // running an ATU that may not be attached.
        sendUserCommand(buildFrameSub(m_session ? m_session->civAddress() : 0xA4,
                                      cmd::kControl, control::kTuner,
                                      std::array<std::uint8_t, 1>{0x02}));
        emit extensionResult(requestId, true);
        return;
    }
    if (verb == QLatin1String("scope.reference")) {
        sendUserCommand(cmdScopeReference(m_session ? m_session->civAddress() : 0xA4,
                                          arg.toDouble()));
        m_scopeCal.referenceDb = arg.toDouble();
        // The reference level shifts the whole trace, so the AXIS has to move
        // with it. Without this the range published at connect goes stale the
        // moment the operator changes the reference — the trace slides and the
        // scale it is drawn against does not, which reads as a calibration
        // error rather than a missing update.
        publishScopeDbmRange();
        emit extensionResult(requestId, true);
        return;
    }
    // TWO VERBS, because there are two different things to say about PC Audio
    // and only one of them is a command.
    //
    // `audio.pc.state` is an OBSERVATION — the client's local audio routing is
    // on or off. It is what the connect edge publishes, and it exists so
    // checkModInput() can advise ("PC Audio is on but DATA OFF MOD is MIC")
    // without the client writing anything. Replaying a client-persisted value
    // onto DATA OFF MOD at connect is what Constitution III forbids in as many
    // words: the radio persists that register itself, so a client that pushes
    // its remembered copy back hands the operator two sources of truth that
    // fight on every reconnect.
    //
    // `audio.pc` is a REQUEST, and only an operator click issues it.
    // Principle II allows exactly that — a user action is a request to the
    // radio — which is why the write lives here and nowhere else.
    if (verb == QLatin1String("audio.pc.state")) {
        m_pcAudioEnabled = arg.toBool();
        checkModInput();
        if (requestId != 0) {
            emit extensionResult(requestId, true);
        }
        return;
    }
    if (verb == QLatin1String("audio.pc")) {
        const auto mod = modulationProfileFor(*m_model);
        if (!mod) {
            // Reachable only from an operator click now that the connect edge
            // publishes state instead of commanding. A warning that answers a
            // request the radio cannot honour is the useful kind — unlike the
            // once-per-session one on a correctly configured radio, which is
            // the one the operator learns to scroll past.
            const QString reason = QStringLiteral(
                "PC Audio cannot select DATA OFF MOD for this Icom model: "
                "its model-specific SET-menu map is not verified.");
            emit configurationWarning(reason);
            if (requestId != 0) {
                emit extensionError(requestId, reason);
            }
            return;
        }
        const bool on = arg.toBool();
        // CAPTURE WHATEVER IS ABOUT TO BE OVERWRITTEN, every time rather than
        // only once. This is the last moment the operator's own selection is
        // observable — after the write the readback reports what we put there.
        //
        // Re-capturing matters because the register is theirs between clicks:
        // an operator who turns PC Audio off and then moves DATA OFF MOD to ACC
        // on the front panel must get ACC back next time, not the USB the
        // session opened on. The link-tick poll keeps m_dataOffModInput current,
        // so the value here is the radio's, not a stale belief.
        //
        // The network source is never captured: putting THAT back on "off"
        // would leave PC Audio off with the radio still listening to the
        // network, which is the state where nothing modulates at all.
        if (m_dataOffModInput >= 0 && m_dataOffModInput != mod->networkOnlyValue) {
            m_dataOffModRestore = m_dataOffModInput;
        }
        m_pcAudioEnabled = on;
        const auto value = static_cast<std::uint8_t>(
            on ? mod->networkOnlyValue
               : m_dataOffModRestore.value_or(mod->micValue));
        sendUserCommand(cmdWriteSetting(
            m_session ? m_session->civAddress() : m_model->civAddress,
            mod->dataOffInputItem, value));
        if (requestId != 0) {
            emit extensionResult(requestId, true);
        }
        return;
    }
    if (verb == QLatin1String("controls.map")) {
        emit extensionResult(requestId, controlMap());
        return;
    }
    if (verb == QLatin1String("controls.meters")) {
        emit extensionResult(requestId, meterMap());
        return;
    }
    if (verb == QLatin1String("controls.scrub")) {
        emit extensionResult(requestId, controlScrub(arg.toString().trimmed()));
        return;
    }
    if (verb == QLatin1String("civ.scheduler.status")) {
        emit extensionResult(requestId, schedulerDiagnostics());
        return;
    }
    if (verb == QLatin1String("civ.scheduler.wait-idle")) {
        int timeoutMs = arg.toMap().value(QStringLiteral("timeoutMs"), 3000).toInt();
        if (!arg.canConvert<QVariantMap>()) {
            timeoutMs = arg.toInt();
            if (timeoutMs <= 0) {
                timeoutMs = 3000;
            }
        }
        timeoutMs = std::clamp(timeoutMs, 0, 10000);
        m_schedulerWaiters.push_back(
            SchedulerWaiter{requestId, nowMs() + timeoutMs});
        serviceSchedulerWaiters(nowMs());
        return;
    }
    if (verb == QLatin1String("civ.trace")) {
        const QString mode = arg.toString().trimmed().toLower();
        emit extensionResult(requestId, civTrace(mode == QLatin1String("all")));
        return;
    }
    if (verb == QLatin1String("civ.session")) {
        QVariantMap result;
        if (m_session) {
            result = m_session->leaseDiagnostics();
        } else {
            result.insert(QStringLiteral("connected"), false);
            result.insert(QStringLiteral("lastRenewalResult"),
                          QStringLiteral("no session"));
        }
        emit extensionResult(requestId, result);
        return;
    }
    if (verb == QLatin1String("civ.send")) {
        // RAW INJECTION. The caller supplies the command bytes ONLY — the
        // preamble, addresses and terminator are ours. That is not politeness:
        // letting a caller write the address fields would let it address a
        // different radio on the bus, or forge a frame that looks like the
        // radio's own reply on the way back through our decoder.
        //
        // Everything after that is unguarded on purpose. This exists to answer
        // "does the radio accept THIS byte sequence", and a version that only
        // permitted sequences we already believed in could not answer it.
        if (!m_session || !m_connected) {
            emit extensionError(requestId, QStringLiteral("not connected"));
            return;
        }
        const auto bytes = parseHexBytes(arg.toString());
        if (!bytes || bytes->empty()) {
            emit extensionError(
                requestId,
                QStringLiteral("civ.send wants hex command bytes, e.g. \"27 15 00 00 00 25 00 00\""));
            return;
        }
        if (bytes->size() + 6 > kMaxCommandFrameBytes) {
            emit extensionError(requestId,
                                QStringLiteral("frame too long (%1 command bytes)")
                                    .arg(bytes->size()));
            return;
        }
        std::vector<std::uint8_t> frame;
        frame.reserve(bytes->size() + 6);
        frame.push_back(kCivPreamble);
        frame.push_back(kCivPreamble);
        frame.push_back(m_session->civAddress());
        frame.push_back(kControllerAddress);
        frame.insert(frame.end(), bytes->begin(), bytes->end());
        frame.push_back(kCivEom);
        sendUserCommand(frame);
        QVariantMap r;
        r.insert(QStringLiteral("sent"), true);
        r.insert(QStringLiteral("bytes"), static_cast<int>(frame.size()));
        emit extensionResult(requestId, r);
        return;
    }
    emit extensionError(requestId, QStringLiteral("unknown verb %1").arg(verb));
}

// ---------------------------------------------------------------------------
// Metering and diagnostics
// ---------------------------------------------------------------------------

void IcomCivBackend::setMeterVisible(MeterId id, bool visible)
{
    m_meters.setVisible(id, visible);
}

void IcomCivBackend::publishMeterDefs()
{
    int index = 0;
    for (const MeterSpec& s : meterSpecs()) {
        MeterDef d;
        d.index = index++;
        d.source = QString::fromUtf8(s.source.data(), static_cast<int>(s.source.size()));
        d.name = QString::fromUtf8(s.name.data(), static_cast<int>(s.name.size()));
        d.unit = QString::fromUtf8(s.unit.data(), static_cast<int>(s.unit.size()));
        d.low = s.low;
        d.high = s.high;
        // The Po meter's high depends on the model's measured curve, and a
        // model we have no curve for must NOT claim watts — see powerCurveFor.
        if (s.id == MeterId::Power) {
            const auto curve = powerCurveFor(*m_model);
            if (curve.empty()) {
                d.unit = QStringLiteral("Percent");
                d.high = 100.0;
            } else {
                d.high = curve.back().value;
            }
        } else if (s.id == MeterId::Id && m_model->civAddress == 0xB6) {
            d.high = 25.0;
        }
        emit meterDefined(d);
    }
}

void IcomCivBackend::onMeterTick()
{
    if (!m_session || !m_connected)
        return;
    const std::int64_t now = nowMs();

    // ASK THE RADIO WHETHER IT IS TRANSMITTING, rather than assuming we are the
    // only thing that can key it.
    //
    // m_keyed was set only by our own setKeying() and by an unsolicited 1C 00
    // frame — which arrives only if CI-V Transceive is on. Key from the
    // radio's own PTT and we never learned, so the TX/RX split kept every
    // transmit meter suppressed and they read as "defined but never fed" while
    // the radio's own meters were plainly moving. That is the operator's
    // report, and it is a receive-side blindness rather than a metering bug.
    if (m_session && now - m_lastPttPollMs >= kPttPollMs) {
        m_lastPttPollMs = now;
        const auto frame = buildFrameSub(m_session->civAddress(), cmd::kControl,
                                         control::kPtt);
        queueRead(frame, "ptt", IcomCivScheduler::Priority::Ptt);
    }

    for (MeterId id : m_meters.due(now)) {
        const MeterSpec* spec = meterSpecFor(id);
        if (!spec)
            continue;
        // Deliberately NOT sendUserCommand(): a meter poll must not reset the
        // scheduler's own user-command guard, or metering would permanently
        // suppress itself.
        const auto frame = cmdReadMeter(m_session->civAddress(), spec->sub);
        queueRead(frame, semanticKey(frame), IcomCivScheduler::Priority::ActiveMeter);
    }
    pumpCiv(now);
}

void IcomCivBackend::onLinkTick()
{
    if (!m_session)
        return;
    const auto s = m_session->stats();

    LinkStats out;
    out.reported = true;
    const quint64 rxPackets = s.control.rxPackets + s.serial.rxPackets + s.audio.rxPackets;
    out.alive = rxPackets > m_link.rxPackets;
    out.rxBytes = static_cast<qint64>(s.control.rxBytes + s.serial.rxBytes + s.audio.rxBytes);
    out.txBytes = static_cast<qint64>(s.control.txBytes + s.serial.txBytes + s.audio.txBytes);
    out.rxPackets = rxPackets;
    out.rxPacketsLost = s.serial.rxLost + s.audio.rxLost;
    // The ping round trip on the CONTROL stream only: the serial and audio
    // streams carry real traffic and their timing is not a clean round trip.
    out.rttMs = s.control.rttMs;

    m_link = out;
    emit linkStatsUpdated(out);

    const IcomCivScheduler::Stats schedulerStats = m_civScheduler.stats();
    if (schedulerStats.timeouts > m_schedulerTimeoutsReported) {
        qCWarning(lcIcomScheduler)
            << "CI-V read timeout; scheduler recovered"
            << "timeouts" << schedulerStats.timeouts
            << "queueDepth" << schedulerStats.queueDepth;
        m_schedulerTimeoutsReported = schedulerStats.timeouts;
    }

    // ---- CI-V STALL DETECTION ------------------------------------------
    //
    // The UDP transport can be perfectly healthy while the COMMAND PLANE is
    // dead: the control stream keeps pinging, rxPackets keeps climbing, and
    // `alive` above stays true, while the radio has answered no CI-V frame for a
    // minute. That happened during this bring-up and cost real time to diagnose
    // — every meter frozen at the same instant, `isConnected()` still true, and
    // nothing anywhere saying so.
    //
    // WHAT MAKES THIS TRIAGEABLE IS THE COMMAND, not the silence. Naming the
    // last frame we sent turns "the radio stopped talking" into "the radio
    // stopped talking after 16 02 02", which is the difference between a bug
    // report and a guess. Logged once per stall, not once per tick, because a
    // warning that repeats every second is one nobody reads.
    if (!m_connected)
        return;
    const qint64 now = nowMs();

    // CI-V Transceive is a low-latency hint, not a subscription. Queue bounded
    // reconciliation groups; the scheduler turns them into one paced stream,
    // coalesces duplicates and lets an operator command overtake all of them.
    const std::uint8_t addr = m_session->civAddress();
    const auto queueControl = [this](const std::vector<std::uint8_t>& frame) {
        queueRead(frame, semanticKey(frame), IcomCivScheduler::Priority::Control);
    };
    const int phase = ++m_controlPollPhase;

    // Switches whose front-panel state must feel live. NR/NB were the measured
    // failure: Transceive sometimes announced them and sometimes did not.
    for (std::uint8_t fn : {func::kAutoNotch, func::kManualNotch,
                            func::kNoiseReduce, func::kNoiseBlanker}) {
        queueControl(cmdReadFunction(addr, fn));
    }

    if (phase % 2 == 0) {
        queueControl(cmdReadFrequency(addr));
        queueControl(m_model->hasVfoModeCommand ? cmdReadVfoMode(addr)
                                                : cmdReadMode(addr));
        for (std::uint8_t fn : {func::kMonitorFn, func::kVox}) {
            queueControl(cmdReadFunction(addr, fn));
        }
    }

    if (phase % 3 == 0) {
        for (std::uint8_t which : {level::kRf, level::kMicGain, level::kMonitor,
                                   level::kVoxGain, level::kNotchPos,
                                   level::kNrLevel, level::kNbLevel}) {
            queueControl(cmdReadLevel(addr, which));
        }
        if (!m_tuning) {
            queueControl(cmdReadLevel(addr, level::kRfPower));
        }
        for (std::uint8_t fn : {func::kPreamp, func::kAgc}) {
            queueControl(cmdReadFunction(addr, fn));
        }
        queueControl(cmdReadAttenuator(addr));
        queueControl(cmdReadTuner(addr));
        for (std::uint8_t sub : {tuneOffset::kFrequency, tuneOffset::kRitOnOff,
                                 tuneOffset::kXitOnOff}) {
            queueControl(cmdReadTuneOffset(addr, sub));
        }
    }
    // SET-menu changes can originate on the front panel. Refresh slowly: they
    // are troubleshooting state, not interactive controls, and share this CI-V
    // stream with tuning and meters.
    if (phase % 12 == 0) {
        if (const auto mod = modulationProfileFor(*m_model)) {
            for (int item : {mod->dataOffInputItem, mod->dataInputItem,
                             mod->usbLevelItem, mod->accessoryLevelItem,
                             mod->networkLevelItem}) {
                if (item >= 0) {
                    queueControl(cmdReadSetting(addr, item));
                }
            }
        }
    }
    pumpCiv(now);
    if (m_lastInboundCivAtMs <= 0) {
        m_lastInboundCivAtMs = now;   // start the clock at the first tick
        return;
    }
    const qint64 silentMs = now - m_lastInboundCivAtMs;
    if (silentMs < kCivStallMs) {
        m_civStallReported = false;
        return;
    }
    if (m_civStallReported)
        return;
    m_civStallReported = true;
    qCWarning(lcIcomLink).noquote()
        << "CI-V STALL: no frame from the radio for" << silentMs << "ms."
        << "Last command sent:" << (m_lastOutboundCiv.isEmpty()
                                        ? QStringLiteral("(none this session)")
                                        : m_lastOutboundCiv)
        << QStringLiteral("%1 ms ago.").arg(m_lastOutboundCivAtMs > 0
                                                ? now - m_lastOutboundCivAtMs : -1)
        << "The transport is still up (rxPackets" << out.rxPackets
        << "), so this is the command plane alone."
        << "Read `civ trace all` for the frames either side of it.";
}

IRadioBackend::HealthSnapshot IcomCivBackend::healthSnapshot() const
{
    HealthSnapshot h;
    h.sections.insert(QStringLiteral("model"), QStringLiteral("Radio"));
    h.values.insert(QStringLiteral("model"),
                    QString::fromUtf8(m_model->name.data(),
                                      static_cast<int>(m_model->name.size())));
    h.labels.insert(QStringLiteral("model"), QStringLiteral("Model"));
    h.order << QStringLiteral("model");

    h.values.insert(QStringLiteral("civ"),
                    QStringLiteral("0x%1").arg(m_model->civAddress, 2, 16, QLatin1Char('0')));
    h.labels.insert(QStringLiteral("civ"), QStringLiteral("CI-V address"));

    // WHERE THE RADIO TAKES ITS MODULATION FROM, plus the level of every source
    // named by that selection. These are separate rows because DATA OFF and
    // DATA are independent radio-owned settings; folding them together hid the
    // exact half responsible for a keyed-but-silent transmission.
    if (const auto mod = modulationProfileFor(*m_model)) {
        const auto describe = [this, &mod](int value) {
            const ModulationInputChoice* selected = nullptr;
            for (const ModulationInputChoice& choice : mod->choices) {
                if (choice.value == value) {
                    selected = &choice;
                    break;
                }
            }
            if (!selected) {
                return QStringLiteral("unknown (%1)").arg(value);
            }
            QString result = QString::fromUtf8(selected->label.data(),
                                               static_cast<int>(selected->label.size()));
            QStringList levels;
            const auto addLevel = [&levels](const QString& name, int percent) {
                levels << (percent >= 0 ? QStringLiteral("%1 %2%").arg(name).arg(percent)
                                        : QStringLiteral("%1 not reported").arg(name));
            };
            if ((selected->sources & ModSourceMic) != 0U) {
                addLevel(QStringLiteral("MIC Gain"),
                         m_micGainReported ? m_micGainPercent : -1);
            }
            if ((selected->sources & ModSourceUsb) != 0U) {
                addLevel(QStringLiteral("USB MOD Level"), m_usbModLevelPercent);
            }
            if ((selected->sources & ModSourceAccessory) != 0U) {
                addLevel(QStringLiteral("ACC MOD Level"), m_accessoryModLevelPercent);
            }
            if ((selected->sources & ModSourceNetwork) != 0U) {
                const QString name = m_model->hasWifi ? QStringLiteral("WLAN MOD Level")
                                                       : QStringLiteral("LAN MOD Level");
                addLevel(name, m_networkModLevelPercent);
            }
            if (!levels.isEmpty()) {
                result += QStringLiteral(" — ") + levels.join(QStringLiteral(", "));
            }
            return result;
        };
        if (m_dataOffModInput >= 0) {
            h.values.insert(QStringLiteral("dataoffmod"), describe(m_dataOffModInput));
            h.labels.insert(QStringLiteral("dataoffmod"), QStringLiteral("DATA OFF MOD"));
            h.order << QStringLiteral("dataoffmod");
        }
        if (m_dataModInput >= 0) {
            h.values.insert(QStringLiteral("datamod"), describe(m_dataModInput));
            h.labels.insert(QStringLiteral("datamod"), QStringLiteral("DATA MOD"));
            h.order << QStringLiteral("datamod");
        }
    }
    h.order << QStringLiteral("civ");

    // THE NEGOTIATED AUDIO RATE, because it is the single biggest thing this
    // session puts on the network and it was previously invisible. 48 kHz
    // uncompressed is 768 kbps each way; on a marginal link that starves both
    // the audio and the CI-V stream sharing it, and an operator debugging
    // "my transmit breaks up" has no way to see which rate they are on.
    h.values.insert(QStringLiteral("audiorate"),
                    QStringLiteral("%1 kHz LPCM (~%2 kbps each way)")
                        .arg(m_audioRateHz / 1000)
                        .arg(m_audioRateHz * 16 / 1000));
    h.labels.insert(QStringLiteral("audiorate"), QStringLiteral("Audio rate"));
    h.order << QStringLiteral("audiorate");

    // RS-BA1 lease state is separate from UDP transport liveness. A rejected
    // or expired token leaves the outer socket answering while CI-V and audio
    // stop, so packet counters alone cannot diagnose this class of freeze.
    if (m_session) {
        const QVariantMap lease = m_session->leaseDiagnostics();
        h.sections.insert(QStringLiteral("lease"), QStringLiteral("RS-BA1 session"));
        h.values.insert(QStringLiteral("lease"),
                        QStringLiteral("%1, %2")
                            .arg(lease.value(QStringLiteral("authenticated")).toBool()
                                     ? QStringLiteral("authenticated")
                                     : QStringLiteral("not authenticated"),
                                 lease.value(QStringLiteral("lastRenewalResult")).toString()));
        h.labels.insert(QStringLiteral("lease"), QStringLiteral("Lease"));
        h.order << QStringLiteral("lease");

        const qint64 ageMs = lease.value(QStringLiteral("lastAcceptedAgeMs")).toLongLong();
        h.values.insert(QStringLiteral("leaseage"),
                        ageMs >= 0 ? QStringLiteral("%1 ms").arg(ageMs)
                                   : QStringLiteral("no accepted token"));
        h.labels.insert(QStringLiteral("leaseage"), QStringLiteral("Last token ACK"));
        h.order << QStringLiteral("leaseage");

        h.values.insert(QStringLiteral("leaseseq"),
                        QStringLiteral("last %1 / next %2 / pending %3")
                            .arg(lease.value(QStringLiteral("lastRenewalSequence")).toUInt())
                            .arg(lease.value(QStringLiteral("nextInnerSequence")).toUInt())
                            .arg(lease.value(QStringLiteral("pendingRenewals")).toInt()));
        h.labels.insert(QStringLiteral("leaseseq"), QStringLiteral("Token sequence"));
        h.order << QStringLiteral("leaseseq");

        h.values.insert(QStringLiteral("leasecounts"),
                        QStringLiteral("%1 accepted / %2 reissued / %3 rejected / %4 stale")
                            .arg(lease.value(QStringLiteral("acceptedRenewals")).toULongLong())
                            .arg(lease.value(QStringLiteral("reissuedTokens")).toULongLong())
                            .arg(lease.value(QStringLiteral("rejectedRenewals")).toULongLong())
                            .arg(lease.value(QStringLiteral("ignoredAuthReplies")).toULongLong()
                                 + lease.value(QStringLiteral("ignoredControlPackets")).toULongLong()));
        h.labels.insert(QStringLiteral("leasecounts"), QStringLiteral("Token replies"));
        h.order << QStringLiteral("leasecounts");
    }

    if (!m_model->verified) {
        // Say so rather than presenting cross-referenced numbers as measured.
        h.values.insert(QStringLiteral("verified"), QStringLiteral("capabilities unverified"));
        h.labels.insert(QStringLiteral("verified"), QStringLiteral("Model data"));
        h.order << QStringLiteral("verified");
    }

    h.sections.insert(QStringLiteral("ovf"), QStringLiteral("Front end"));
    h.values.insert(QStringLiteral("ovf"), m_overflow ? QStringLiteral("OVERLOAD")
                                                      : QStringLiteral("ok"));
    h.labels.insert(QStringLiteral("ovf"), QStringLiteral("ADC overflow"));
    h.order << QStringLiteral("ovf");

    // Vd and Id only if the radio has actually reported them. A key absent from
    // `values` renders as "not reported", which is genuinely different from 0 V.
    if (m_vdVolts > 0.0) {
        h.values.insert(QStringLiteral("vd"), QStringLiteral("%1 V").arg(m_vdVolts, 0, 'f', 1));
        h.labels.insert(QStringLiteral("vd"), QStringLiteral("PA supply"));
        h.order << QStringLiteral("vd");
    }
    if (m_idAmps > 0.0) {
        h.values.insert(QStringLiteral("id"), QStringLiteral("%1 A").arg(m_idAmps, 0, 'f', 2));
        h.labels.insert(QStringLiteral("id"), QStringLiteral("PA current"));
        h.order << QStringLiteral("id");
    }
    // NO PA TEMPERATURE. The IC-705 does not report one, and the key is omitted
    // rather than reported as zero.
    return h;
}

IRadioBackend::LinkStats IcomCivBackend::linkStats() const { return m_link; }

}  // namespace AetherSDR::icom
