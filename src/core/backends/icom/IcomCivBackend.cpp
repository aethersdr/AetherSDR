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

// Metering is examined this often; the MeterPoller decides what is actually
// due. Deliberately faster than the fastest meter interval so a due meter is
// not delayed by up to a whole tick.
constexpr int kMeterTickMs = 40;
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

// AetherSDR's slider is 0..100; the radio's register is 0..255.
int percentToRaw(int percent) { return std::clamp(percent, 0, 100) * 255 / 100; }

}  // namespace

IcomCivBackend::IcomCivBackend(QObject* parent)
    : IRadioBackend(parent), m_model(&unknownModel())
{
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
    p.civAddress = static_cast<std::uint8_t>(
        request.params.value(QStringLiteral("icom.civAddress"), 0xA4).toUInt());
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
    for (QTimer** t : {&m_meterTimer, &m_linkTimer}) {
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
    m_framesObserved = 0;
    m_tuning = false;
    if (m_connected) {
        m_connected = false;
        emit disconnected();
    }
}

bool IcomCivBackend::isConnected() const { return m_connected; }

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
    if (const IcomModel* byName = modelForName(deviceName.toStdString()))
        m_model = byName;

    // The radio's audio is 48 kHz mono; the seam's per-slice contract is 24 kHz
    // interleaved stereo. Built once here rather than per-buffer: r8brain is
    // stateful, and a fresh instance per callback restarts its filter history
    // every block, which is audible as a periodic tick.
    m_rxResampler = std::make_unique<Resampler>(
        static_cast<double>(m_audioRateHz), static_cast<double>(kEngineAudioRateHz), 4096);

    // ASK the radio what it is. The CI-V address is user-changeable and several
    // models speak this same transport, so a hardcoded 0xA4 would silently
    // mis-decode an IC-9700 someone pointed this at.
    m_session->sendCiv(cmdReadId(m_session->civAddress()));
    m_session->sendCiv(cmdReadFrequency(m_session->civAddress()));
    m_session->sendCiv(cmdReadMode(m_session->civAddress()));

    // ASK WHERE THE RADIO TAKES ITS MODULATION FROM. Not cosmetic: if this is
    // not WLAN, everything else about transmit can be perfect and the operator
    // still gets zero output. Diagnosing it from the outside means noticing
    // that a keyed radio with healthy audio counters makes no power, which is
    // exactly the dead end this avoids.
    m_session->sendCiv(cmdReadSetting(m_session->civAddress(),
                                      setting::kDataOffModInput));
    m_session->sendCiv(cmdReadSetting(m_session->civAddress(),
                                      setting::kDataModInput));

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
                               level::kMicGain, level::kCompLevel,
                               level::kNrLevel, level::kNbLevel,
                               level::kNotchPos, level::kRf, level::kVoxGain})
        m_session->sendCiv(cmdReadLevel(m_session->civAddress(), which));

    // ...and the switches, which have the same problem: the applet toggles all
    // read "off" on a radio that may have NR or the compressor running.
    for (std::uint8_t fn : {func::kPreamp, func::kAgc, func::kNoiseReduce,
                            func::kNoiseBlanker, func::kAutoNotch,
                            func::kManualNotch,
                            func::kCompressor, func::kMonitorFn, func::kVox})
        m_session->sendCiv(cmdReadFunction(m_session->civAddress(), fn));

    // The attenuator is NOT sub-addressed, so it needs its own read rather than
    // a slot in the loop above.
    m_session->sendCiv(cmdReadAttenuator(m_session->civAddress()));

    // RIT / XIT and the antenna tuner. All four were write-only: the controls
    // opened at OUR defaults, so an operator who set RIT on the radio and
    // reconnected saw zero on a rig that was still offset.
    for (std::uint8_t sub : {tuneOffset::kFrequency, tuneOffset::kRitOnOff,
                             tuneOffset::kXitOnOff})
        m_session->sendCiv(cmdReadTuneOffset(m_session->civAddress(), sub));
    m_session->sendCiv(cmdReadTuner(m_session->civAddress()));

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


}

void IcomCivBackend::onSessionDisconnected(const QString& reason)
{
    const bool was = m_connected;
    m_connected = false;
    if (was)
        emit disconnected();
    if (!reason.isEmpty())
        emit connectionError(reason);
}

void IcomCivBackend::checkModInput()
{
    // Report ONCE both answers are in, and only when something is actually
    // wrong. A warning that fires on a correctly configured radio is one the
    // operator learns to scroll past (CERTIFICATION.md 1.28).
    if (m_dataOffModInput < 0 || m_dataModInput < 0)
        return;

    // ONLY a radio with Wi-Fi has a WLAN modulation source to select.
    //
    // The 1A 05 item numbers (118/119) and the value table below are read from
    // ONE model's CI-V Reference Guide and sent to every Icom, but each model
    // numbers its own SET menu and its own enum. On an IC-9700 — LAN only, no
    // Wi-Fi — both items were set correctly on the front panel and the radio
    // answered 0x01, which this table calls "USB". So either 118/119 are not
    // MOD Input on that model, or 0x01 IS its network source; either way
    // demanding 0x03 asks for a setting the radio cannot offer, and the warning
    // could never be satisfied by any front-panel action.
    //
    // Reported by an operator with the radio in front of them (2026-08-05): set
    // to LAN on both, warned anyway, every session. A check that fires on a
    // correctly configured radio is worse than no check — it is the one the
    // operator learns to scroll past, and it trains them past the real ones.
    //
    // Staying silent here loses nothing that was working: the warning was
    // WRONG on this radio, not merely noisy. Re-enable per model once the
    // mapping is confirmed against that model's own guide (the same bar
    // IcomModel::verified sets for the rest of the table).
    // Note this also silences the check on an UNIDENTIFIED radio, since
    // kUnknown carries hasWifi=false. That is the right outcome, though for a
    // second reason: kUnknown is also hasTransmit=false, and a radio this
    // client will not let key has no modulation path to warn about. Warning
    // there would be advice about a transmission that cannot happen, decoded
    // through a value table not known to apply to that model.
    if (!m_model->hasWifi)
        return;

    const bool voiceOk = m_dataOffModInput == setting::kModWlan;
    const bool dataOk  = m_dataModInput == setting::kModWlan;
    if (voiceOk && dataOk)
        return;

    auto name = [](int v) -> QString {
        switch (v) {
        case setting::kModMic:    return QStringLiteral("MIC");
        case setting::kModUsb:    return QStringLiteral("USB");
        case setting::kModMicUsb: return QStringLiteral("MIC+USB");
        case setting::kModWlan:   return QStringLiteral("WLAN");
        default:                  return QStringLiteral("unknown(%1)").arg(v);
        }
    };

    QStringList wrong;
    if (!voiceOk)
        wrong << QStringLiteral("voice modes take modulation from %1")
                     .arg(name(m_dataOffModInput));
    if (!dataOk)
        wrong << QStringLiteral("data modes take modulation from %1")
                     .arg(name(m_dataModInput));

    // A configurationWarning, NOT a connectionError: this is advice about a
    // radio that is otherwise working perfectly. connectionError is fatal to
    // every consumer — RadioModel starts its reconnect timer on it — so raising
    // it here dropped the session ~4 ms after the CI-V stream came live and
    // reconnected into the same check forever. The operator saw a radio that
    // would not stay connected and a message about a menu setting, with no way
    // to tell that the message WAS the disconnect.
    //
    // It still reaches the operator; it just no longer costs them the session.
    emit configurationWarning(
        QStringLiteral("The radio is not listening to network audio — %1. "
                       "AetherSDR's transmit audio will be ignored and the radio "
                       "will key at zero output. On the radio: MENU > SET > "
                       "Connectors > MOD Input > set DATA OFF MOD and DATA MOD "
                       "to WLAN.")
            .arg(wrong.join(QStringLiteral(", "))));
}

void IcomCivBackend::applyScopeStartup()
{
    if (!m_session || !m_model->hasScope)
        return;
    // BOTH switches. Enabling only 0x27 0x10 turns the scope on the radio's own
    // screen and sends us nothing — the number-one "black panadapter" cause.
    m_session->sendCiv(cmdScopeOnOff(m_session->civAddress(), true));
    m_session->sendCiv(cmdScopeDataOutput(m_session->civAddress(), true));
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

    noteControlSeen(frame.cmd, frame.sub, frame.hasSub);

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
        traceCiv(/*outbound=*/false, flat);
    }

    switch (frame.cmd) {
    case cmd::kReadId: {
        if (auto addr = parseModelIdReply(frame)) {
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
            }
            RadioDelta r;
            r.model = QString::fromUtf8(m_model->name.data(),
                                        static_cast<int>(m_model->name.size()));
            emit radioChanged(r);
        }
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
        const QString neutral = QString::fromStdString(modeToNeutral(m_mode, m_dataMode));
        if (neutral.isEmpty())
            return;   // D-STAR: a waveform, not a demodulator setting
        SliceDelta s;
        s.mode = neutral;
        // The passband travels WITH the mode, in the same delta, because the
        // radio will never send one. Applied after the mode by SliceModel's own
        // ordering, which is what stops a narrow CW window surviving into DIGU.
        const auto [low, high] =
            passbandForModeAndFilter(currentLadderMode().toStdString(), m_filter);
        s.filterLow  = low;
        s.filterHigh = high;
        emit sliceChanged(sliceId(), s);
        // The filter LADDER changes with the mode, so the buttons have to be
        // rebuilt from the new one. Change-gated inside the models, so the
        // repeat this produces on an unchanged mode costs nothing.
        publishCapabilities();
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
        // 0..255 back to the 0..100 every AetherSDR control uses.
        const int pct = std::clamp((*raw * 100 + 127) / 255, 0, 100);
        switch (frame.sub) {
        case level::kRfPower: {
            m_txPowerPercent = pct;
            TransmitDelta t; t.rfPower = pct;
            emit transmitChanged(t);
            return;
        }
        case level::kMicGain: {
            m_micGainPercent = pct;
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

    case cmd::kSetting: {
        // 1A 05 <item hi> <item lo> <value>
        if (!frame.hasSub || frame.sub != 0x05 || frame.data.size() < 3)
            return;
        const int item = decodeBcdByte(frame.data[0]) * 100 + decodeBcdByte(frame.data[1]);
        const int value = frame.data[2];
        if (item == setting::kDataOffModInput)
            m_dataOffModInput = value;
        else if (item == setting::kDataModInput)
            m_dataModInput = value;
        else
            return;
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

        m_meters.markAnswered(spec->id, QDateTime::currentMSecsSinceEpoch());
        const double value = meterValue(spec->id, *raw, s9ReferenceFor(m_frequencyHz));

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
            t.atuEnabled = (v != 0);
            t.atuStatusRaw = v == 0x02 ? QStringLiteral("TUNING")
                           : v == 0x01 ? QStringLiteral("SUCCESSFUL")
                                       : QStringLiteral("NONE");
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

void IcomCivBackend::submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz)
{
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

void IcomCivBackend::sendUserCommand(const std::vector<std::uint8_t>& frame)
{
    if (!m_session || !m_connected)
        return;
    // Tell the scheduler a real command just went out, so metering yields and
    // the command is not stuck behind a queue of polls.
    m_meters.noteUserCommand(QDateTime::currentMSecsSinceEpoch());
    traceCiv(/*outbound=*/true, frame);
    // Record WHICH registry row this frame belongs to. Byte 4 is the command and
    // byte 5 the subcommand when the row has one — the same layout buildFrame
    // writes. This is what turns the registry's declared wiring into an observed
    // fact: `controls.map` can then say a row claims to be sent AND has been.
    if (frame.size() > 5)
        noteControlSent(frame[4], frame[5], true);
    else if (frame.size() > 4)
        noteControlSent(frame[4], 0, false);
    // Remembered for the stall warning in onLinkTick: what was the radio last
    // asked to do before it went quiet.
    if (frame.size() > 4) {
        QString hex;
        for (std::size_t i = 4; i + 1 < frame.size(); ++i)
            hex += QStringLiteral("%1 ").arg(frame[i], 2, 16, QLatin1Char('0'));
        m_lastOutboundCiv = hex.trimmed();
        m_lastOutboundCivAtMs = QDateTime::currentMSecsSinceEpoch();
    }
    m_session->sendCiv(frame);
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
    m_dataMode = data;
    // KEEP THE FILTER SLOT across a mode change. Hardcoding FIL1 here meant
    // every mode change jumped to the widest filter, so an operator working a
    // narrow CW filter lost it the moment they visited another mode and came
    // back.
    sendUserCommand(cmdSetMode(m_session ? m_session->civAddress() : 0xA4, *civ, m_filter));

    // PUBLISH THE PASSBAND NOW, from the mode we just commanded.
    //
    // Waiting for the radio to report the mode back is not good enough: the
    // report only arrives if CI-V Transceive is on, and even then it lands
    // milliseconds later. radiocert's passband-after-mode-change stage caught
    // exactly that — CW then DIGU left the window at the previous mode's width,
    // so a decoder in a wide mode saw a narrow slot. The radio owns its DSP and
    // sends no passband, so this is the only place it can come from.
    const auto [low, high] = passbandForModeAndFilter(mode.toStdString(), m_filter);
    SliceDelta d;
    d.mode = mode.toUpper();
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
    sendUserCommand(cmdSetMode(m_session ? m_session->civAddress() : 0xA4, m_mode, filter));

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
                                level::kRf, percentToRaw(std::clamp(gainDb, 0, 100))));
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
    sendUserCommand(cmdSetLevel(m_session ? m_session->civAddress() : 0xA4,
                                level::kMicGain, percentToRaw(gainPercent)));
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
        sendUserCommand(cmdSetLevel(addr, level::kNrLevel, percentToRaw(level)));
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
        sendUserCommand(cmdSetLevel(addr, level::kNbLevel, percentToRaw(level)));
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
    sendUserCommand(cmdSetLevel(addr, level::kNotchPos, percentToRaw(position)));
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
                                level::kAf, percentToRaw(m_afGainPercent)));
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
    // Same rule as NR and NB: the threshold register survives the function
    // being switched off, so pushing it while disabled changes what the
    // operator gets back when they re-enable.
    if (on)
        sendUserCommand(cmdSetLevel(addr, level::kVoxGain, percentToRaw(level)));
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
    // Ask what it did. The radio does not report the outcome unprompted, and
    // "tuning" is a transient the operator needs to see end.
    if (m_session)
        m_session->sendCiv(cmdReadTuner(m_session->civAddress()));
}

void IcomCivBackend::setSliceSquelch(int, bool on, int level)
{
    m_squelchPercent = on ? level : 0;
    // NO SQUELCH ENABLE EXISTS on this radio — the threshold IS the control,
    // and squelch is "off" when it sits at zero. Mapping the UI's toggle onto
    // the threshold is the only honest translation available; the alternative
    // is a switch that does nothing.
    sendUserCommand(cmdSetLevel(m_session ? m_session->civAddress() : 0xA4,
                                level::kSquelch, on ? percentToRaw(level) : 0));
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
    if (on && tunePowerPercent >= 0)
        setTxPower(tunePowerPercent);
    // Raise the tone BEFORE keying and drop it after, so no part of the keyed
    // window is silent — a tuner that samples during a silent leading edge
    // reads infinite SWR and some will refuse to start.
    m_tuning = on;
    if (on)
        m_tunePhase = 0.0;
    setKeying(on);
}

void IcomCivBackend::setTxPower(int percent)
{
    m_txPowerPercent = std::clamp(percent, 0, 100);
    sendUserCommand(cmdSetLevel(m_session ? m_session->civAddress() : 0xA4,
                                level::kRfPower, percentToRaw(m_txPowerPercent)));
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

    for (const auto& c : icom::controlSpecs()) {
        if (c.cmd != setCmd)
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

void IcomCivBackend::noteControlSeen(std::uint8_t cmd, std::uint8_t sub, bool hasSub)
{
    ++m_framesObserved;
    m_lastInboundCivAtMs = QDateTime::currentMSecsSinceEpoch();
    forEachSpecForFrame(cmd, sub, hasSub, [this](const icom::ControlSpec& c) {
        const QString id = QString::fromUtf8(c.id.data(), static_cast<int>(c.id.size()));
        m_controlsSeen.insert(id);
        // The radio answered for this row, so the decode above adopted its
        // value into the scrub mirror. This is the OTHER half of "we know what
        // this control is set to" — the half that does not require the operator
        // to have touched it. Only sendCiv-issued connect reads reach here;
        // they are the reads whose answers populate the mirrors.
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
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

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

        // DRIVE IT THROUGH THE SEAM, with a value that changes nothing.
        //
        // Re-asserting the current value is the whole trick: the question is
        // "does this intent reach the wire", not "does the radio obey", and a
        // scrub that moved every control would leave the operator's radio
        // rearranged.
        const bool driven = scrubDrive(c);
        const bool onWire = m_controlsSent.contains(id);
        if (onWire)
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
        r.insert(QStringLiteral("status"),
                 onWire    ? QStringLiteral("LINKED")
                 : !driven ? QStringLiteral("NOT-TESTED")
                           : QStringLiteral("BROKEN"));
        r.insert(QStringLiteral("verdict"),
                 onWire
                     ? QStringLiteral("the seam verb put this command on the wire")
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
                              "never scrubbed."));
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
    if (id == QLatin1String("squelch"))  { setSliceSquelch(slice, m_squelchPercent > 0, m_squelchPercent); return true; }
    if (id == QLatin1String("agc"))      { setSliceAgc(slice, m_agcMode, 0); return true; }
    if (id == QLatin1String("tx.power")) { setTxPower(m_txPowerPercent); return true; }
    if (id == QLatin1String("mic.gain")) { setMicGain(m_micGainPercent); return true; }
    if (id == QLatin1String("monitor"))  { setTxAudioMonitor(m_monitorOn); return true; }
    if (id == QLatin1String("af.gain"))  { setSliceAudioGain(slice, m_afGainPercent); return true; }

    if (id == QLatin1String("vox") || id == QLatin1String("vox.gain")) {
        // The gain register only goes out while VOX is enabled, same rule as
        // NR and NB.
        if (id == QLatin1String("vox.gain") && m_voxEnableSent != 1)
            return false;
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
    if (id == QLatin1String("mode") || id == QLatin1String("filter")) {
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

void IcomCivBackend::traceCiv(bool outbound, std::span<const std::uint8_t> frame)
{
    QString hex;
    hex.reserve(static_cast<int>(frame.size()) * 3);
    for (std::uint8_t b : frame) {
        if (!hex.isEmpty())
            hex += QLatin1Char(' ');
        hex += QStringLiteral("%1").arg(b, 2, 16, QLatin1Char('0'));
    }
    m_civTrace.push_back({QDateTime::currentMSecsSinceEpoch(), outbound, hex});
    while (m_civTrace.size() > kCivTraceMax)
        m_civTrace.pop_front();
}

QVariantList IcomCivBackend::civTrace(bool includeRoutine) const
{
    const std::int64_t now = QDateTime::currentMSecsSinceEpoch();
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
        if (!includeRoutine && !e.outbound) {
            if (e.hex.startsWith(QLatin1String("15 "))
                || e.hex.startsWith(QLatin1String("1c 00"))) {
                continue;
            }
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
    if (verb == QLatin1String("civ.trace")) {
        const QString mode = arg.toString().trimmed().toLower();
        emit extensionResult(requestId, civTrace(mode == QLatin1String("all")));
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
        }
        emit meterDefined(d);
    }
}

void IcomCivBackend::onMeterTick()
{
    if (!m_session || !m_connected)
        return;
    const std::int64_t now = QDateTime::currentMSecsSinceEpoch();

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
        m_session->sendCiv(buildFrameSub(m_session->civAddress(), cmd::kControl,
                                         control::kPtt));
    }

    for (MeterId id : m_meters.due(now)) {
        const MeterSpec* spec = meterSpecFor(id);
        if (!spec)
            continue;
        // Deliberately NOT sendUserCommand(): a meter poll must not reset the
        // scheduler's own user-command guard, or metering would permanently
        // suppress itself.
        m_session->sendCiv(cmdReadMeter(m_session->civAddress(), spec->sub));
    }
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
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
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

    // WHERE THE RADIO TAKES ITS MODULATION FROM. On a health readout because
    // "keys but makes no power" has no other visible cause: the audio counters
    // climb, the meters are fresh, and the modulator is listening elsewhere.
    if (m_dataOffModInput >= 0 || m_dataModInput >= 0) {
        auto name = [](int v) -> QString {
            switch (v) {
            case setting::kModMic:    return QStringLiteral("MIC");
            case setting::kModUsb:    return QStringLiteral("USB");
            case setting::kModMicUsb: return QStringLiteral("MIC+USB");
            case setting::kModWlan:   return QStringLiteral("WLAN");
            default:                  return QStringLiteral("?");
            }
        };
        // The verdict, like the warning in checkModInput(), is only meaningful
        // on a radio that HAS a WLAN source. Elsewhere show the raw values and
        // pass no judgement: an IC-9700 set correctly to LAN reads back 0x01
        // here, and appending "NOT WLAN" to that is telling the operator their
        // working radio is misconfigured.
        const bool ok = !m_model->hasWifi
                        || (m_dataOffModInput == setting::kModWlan
                            && m_dataModInput == setting::kModWlan);
        h.values.insert(QStringLiteral("modinput"),
                        QStringLiteral("%1 voice / %2 data%3")
                            .arg(name(m_dataOffModInput), name(m_dataModInput),
                                 ok ? QString() : QStringLiteral("  — NOT WLAN")));
        h.labels.insert(QStringLiteral("modinput"), QStringLiteral("MOD Input"));
        h.order << QStringLiteral("modinput");
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
