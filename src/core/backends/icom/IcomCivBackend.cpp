#include "core/backends/icom/IcomCivBackend.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cmath>

#include "core/Resampler.h"

namespace AetherSDR::icom {
namespace {

// The pan intents are the two that most need to say what they DECIDED rather
// than what they were asked, because both of them deliberately do something
// other than the literal request: one refuses, the other quantises.
Q_LOGGING_CATEGORY(lcIcomPan, "aether.icom.pan")

// Metering is examined this often; the MeterPoller decides what is actually
// due. Deliberately faster than the fastest meter interval so a due meter is
// not delayed by up to a whole tick.
constexpr int kMeterTickMs = 40;
// Transport counters publish on a FIXED cadence, not on receive: "nothing
// arrived this second" is the observation the heartbeat's alarm path waits for,
// and a backend that emits only on receive can never report its own silence.
constexpr int kLinkTickMs = 1000;

QByteArray floatBytes(const std::vector<float>& v)
{
    return {reinterpret_cast<const char*>(v.data()),
            static_cast<qsizetype>(v.size() * sizeof(float))};
}

// AetherSDR's slider is 0..100; the radio's register is 0..255.
int percentToRaw(int percent) { return std::clamp(percent, 0, 100) * 255 / 100; }

// Default RX passband per mode, in Hz relative to the carrier. Sign carries the
// sideband, matching SliceModel's convention.
//
// THE BACKEND MUST SUPPLY THIS. radiocert's passband-after-mode-change stage
// found it missing on the first run: CW -> DIGU left the window at -1500..1500,
// so a decoder in a wide mode saw a narrow slot. A radio that owns its own DSP
// sends no passband echo to heal that, and the IC-705's three fixed IF filters
// cannot be read back as Hz — so nothing else in the chain can fill it in.
//
// These are the radio's own defaults for each mode, not arbitrary picks.
std::pair<int, int> defaultPassbandFor(const QString& mode)
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("LSB"))  return {-2700, -300};
    if (u == QLatin1String("USB"))  return {300, 2700};
    if (u == QLatin1String("DIGL")) return {-3000, -150};
    if (u == QLatin1String("DIGU")) return {150, 3000};
    if (u == QLatin1String("RTTY")) return {-3000, -150};
    // CW is symmetric about the pitch; the radio centres its filter on the tone.
    if (u == QLatin1String("CW") || u == QLatin1String("CWU")
        || u == QLatin1String("CWL"))
        return {-250, 250};
    if (u == QLatin1String("AM"))   return {-4500, 4500};
    if (u == QLatin1String("FM") || u == QLatin1String("NFM")) return {-7000, 7000};
    if (u == QLatin1String("WFM"))  return {-100000, 100000};
    return {-1500, 1500};
}

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

    // NO IQ, on any networked Icom. Not deferred — absent. See icom-oracle §8.1.
    c.hasDaxStreams = false;

    // The radio HAS a GPS and the protocol will not carry its data.
    c.hasGpsLocation = false;

    c.hasSupplyVoltageTelemetry = true;   // 0x15 0x15 Vd

    // No internal ATU on the IC-705. `1C 01` drives an EXTERNAL AH-705 and
    // there is no command to detect whether one is attached, so the capability
    // is unanswerable from the radio; false is the safer default.
    c.hasTuner = false;

    // The radio chooses its own modulation input from its own menu (MOD Input
    // > DATA MOD, which must be WLAN for us to be heard at all). A client
    // cannot pick MIC / BAL / LINE / ACC, so the Phone applet collapses to PC.
    c.hasSelectableMicInputs = false;

    // THREE, and only three. filterForWidthHz() already snaps a request onto
    // them; this is what stops the UI offering widths that all land on the same
    // filter. The values are the radio's own SSB defaults, which the operator
    // can redefine in its SET menu and we cannot read back — so these are the
    // best available labels, not a promise about the passband.
    if (m_model->hasScope || m_model->isKnown())
        c.rxFilterWidthsHz = {1800, 2400, 3000};

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
    p.sampleRateHz = kRadioAudioRateHz;

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
    m_nrEnableSent = m_nbEnableSent = m_anfEnableSent = -1;
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
        static_cast<double>(kRadioAudioRateHz), static_cast<double>(kEngineAudioRateHz), 4096);

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
                               level::kNrLevel, level::kNbLevel})
        m_session->sendCiv(cmdReadLevel(m_session->civAddress(), which));

    // ...and the switches, which have the same problem: the applet toggles all
    // read "off" on a radio that may have NR or the compressor running.
    for (std::uint8_t fn : {func::kPreamp, func::kAgc, func::kNoiseReduce,
                            func::kNoiseBlanker, func::kAutoNotch,
                            func::kCompressor, func::kMonitorFn, func::kVox})
        m_session->sendCiv(cmdReadFunction(m_session->civAddress(), fn));

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

    // The RF-gain control is a THREE-POSITION preamp, not a dB register.
    // Advertising the real, discrete range is what makes the existing slider
    // snap to three detents instead of sweeping smoothly over a control that
    // cannot follow it.
    emit panRfGainInfoChanged(panId(), 0, 2, 1);

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
        const QString neutral = QString::fromStdString(modeToNeutral(m_mode, m_dataMode));
        if (neutral.isEmpty())
            return;   // D-STAR: a waveform, not a demodulator setting
        SliceDelta s;
        s.mode = neutral;
        // The passband travels WITH the mode, in the same delta, because the
        // radio will never send one. Applied after the mode by SliceModel's own
        // ordering, which is what stops a narrow CW window surviving into DIGU.
        const auto [low, high] = defaultPassbandFor(neutral);
        s.filterLow  = low;
        s.filterHigh = high;
        emit sliceChanged(sliceId(), s);
        return;
    }

    // THE RADIO'S OWN LEVELS AND SWITCHES, adopted into the models.
    //
    // These arrive as answers to the connect-time reads above, and also
    // unsolicited whenever the operator turns a knob on the radio — the same
    // decode serves both, which is what keeps the UI honest while someone is
    // standing at the rig.
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
            TransmitDelta t; t.micLevel = pct;
            emit transmitChanged(t);
            return;
        }
        case level::kCompLevel: {
            // The radio's 0..10 compressor mapped back onto NOR/DX/DX+.
            TransmitDelta t;
            t.speechProcLevel = pct;
            emit transmitChanged(t);
            return;
        }
        case level::kAf: {
            SliceDelta d; d.audioGain = pct;
            emit sliceChanged(sliceId(), d);
            return;
        }
        case level::kSquelch: {
            SliceDelta d;
            d.squelchLevel = pct;
            // NO SEPARATE ENABLE on this radio — the threshold IS the control,
            // so a non-zero threshold is what "squelch on" means here.
            d.squelchOn = pct > 0;
            emit sliceChanged(sliceId(), d);
            return;
        }
        case level::kNrLevel: {
            SliceDelta d; d.nrLevel = pct;
            emit sliceChanged(sliceId(), d);
            return;
        }
        case level::kNbLevel: {
            SliceDelta d; d.nbLevel = pct;
            emit sliceChanged(sliceId(), d);
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
        case func::kCompressor: {
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
            emit sliceChanged(sliceId(), d);
            return;
        }
        case func::kPreamp: {
            SliceDelta d; d.rfGain = v;
            emit sliceChanged(sliceId(), d);
            return;
        }
        default:
            return;
        }
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
        auto raw = decodeLevel(frame.data);
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
    if (sampleRateHz != kRadioAudioRateHz) {
        if (sampleRateHz <= 0)
            return;
        // Built once and kept: r8brain is stateful, and a fresh instance per
        // buffer restarts its filter history every block — audible as a tick at
        // the block rate, and on a transmit path that goes on the air.
        if (!m_txResampler || m_txResamplerFromHz != sampleRateHz) {
            m_txResamplerFromHz = sampleRateHz;
            m_txResampler = std::make_unique<Resampler>(
                static_cast<double>(sampleRateHz),
                static_cast<double>(kRadioAudioRateHz), 4096);
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
        return;
    }
    m_dataMode = data;
    sendUserCommand(cmdSetMode(m_session ? m_session->civAddress() : 0xA4, *civ, 1));

    // PUBLISH THE PASSBAND NOW, from the mode we just commanded.
    //
    // Waiting for the radio to report the mode back is not good enough: the
    // report only arrives if CI-V Transceive is on, and even then it lands
    // milliseconds later. radiocert's passband-after-mode-change stage caught
    // exactly that — CW then DIGU left the window at the previous mode's width,
    // so a decoder in a wide mode saw a narrow slot. The radio owns its DSP and
    // sends no passband, so this is the only place it can come from.
    const auto [low, high] = defaultPassbandFor(mode);
    SliceDelta d;
    d.mode = mode.toUpper();
    d.filterLow  = low;
    d.filterHigh = high;
    emit sliceChanged(sliceId(), d);
}

void IcomCivBackend::setSliceFilter(int, int lowHz, int highHz)
{
    // The radio has three fixed IF filters, not a continuous passband, so this
    // can only SNAP. What the radio actually took comes back on its own mode
    // report — we must not echo the requested width as if it were applied.
    const int width = std::abs(highHz - lowHz);
    const int filter = filterForWidthHz(width);
    sendUserCommand(cmdSetMode(m_session ? m_session->civAddress() : 0xA4, m_mode, filter));
}

void IcomCivBackend::setSliceAgc(int, const QString& mode, int)
{
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

void IcomCivBackend::setPanCenter(const QString&, double hz)
{
    Q_UNUSED(hz);

    // THE PAN CENTRE IS NOT OURS TO SET, and this used to retune the radio.
    //
    // In centre mode the scope window is slaved to the operating frequency —
    // the radio offers no way to offset one from the other — so this used to
    // forward to setSliceFrequency() on the reasoning that moving the window IS
    // retuning. That reasoning is backwards from the operator's. Dragging a
    // panadapter is a request to LOOK somewhere, and on every other backend it
    // moves the view while the slice stays put. Here it moved the radio: a drag
    // across the waterfall walked the VFO off frequency, and because zoom
    // dispatches centre and bandwidth together, so did every zoom click.
    //
    // So: refuse, and re-assert the truth IMMEDIATELY rather than waiting for
    // the next sweep to contradict the view. Without the re-assert the widget
    // keeps its optimistic centre for up to a frame and the trace visibly
    // slides before snapping back.
    //
    // The honest alternative would be the radio's FIXED scope mode, whose
    // window is genuinely independent of the VFO. It is not reachable from
    // here: its edges are not free-form but three saved presets per band
    // (0x27 0x1E), so following a drag would overwrite the operator's own
    // stored scope edges thirty times a second.
    qCDebug(lcIcomPan) << "pan-centre request REFUSED (the scope is slaved to the VFO);"
                       << "asked" << hz << "Hz, radio is at" << m_scopeCentreHz << "Hz";
    if (m_scopeSpanHz <= 0)
        return;

    // QUEUED, and that is not incidental. RadioModel writes the REQUESTED centre
    // into the pan model on the line after it calls us, so a direct emit here is
    // overwritten by the very value we are refusing. Deferring to the next event
    // loop iteration puts the correction after that write and still lands inside
    // the same frame — sooner than the next sweep would, which is the whole
    // reason to re-assert at all rather than just waiting 33 ms.
    const double centreMhz = static_cast<double>(m_scopeCentreHz) / 1e6;
    const double widthMhz  = static_cast<double>(m_scopeSpanHz * 2) / 1e6;
    QMetaObject::invokeMethod(this, [this, centreMhz, widthMhz] {
        emit panCenterBandwidthChanged(panId(), centreMhz, widthMhz);
    }, Qt::QueuedConnection);
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

void IcomCivBackend::setPanRfGain(const QString&, int gainDb)
{
    // There is NO continuous RF-gain register. The IC-705 has a three-position
    // preamp, so this snaps to it; panRfGainInfoChanged advertises (0, 2, 1) so
    // the slider stops where the hardware does instead of sweeping smoothly
    // over a control that has three detents.
    const int preamp = std::clamp(gainDb, 0, 2);
    sendUserCommand(cmdSetFunction(m_session ? m_session->civAddress() : 0xA4,
                                   func::kPreamp, preamp));
}

void IcomCivBackend::setSpeechProcessor(bool on, int level)
{
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
    sendUserCommand(cmdSetLevel(m_session ? m_session->civAddress() : 0xA4,
                                level::kMicGain, percentToRaw(gainPercent)));
}

void IcomCivBackend::setTxAudioMonitor(bool on)
{
    // The FUNCTION only. The radio has a separate monitor LEVEL (14 15) and no
    // seam verb carries it, so setting it here would either overwrite whatever
    // the operator dialled in on the radio or invent a value — both worse than
    // leaving their own setting alone and toggling what was actually asked for.
    sendUserCommand(cmdSetFunction(m_session ? m_session->civAddress() : 0xA4,
                                   func::kMonitorFn, on ? 1 : 0));
}

void IcomCivBackend::setSliceNoiseReduction(int, bool on, int level)
{
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

void IcomCivBackend::setSliceSquelch(int, bool on, int level)
{
    // NO SQUELCH ENABLE EXISTS on this radio — the threshold IS the control,
    // and squelch is "off" when it sits at zero. Mapping the UI's toggle onto
    // the threshold is the only honest translation available; the alternative
    // is a switch that does nothing.
    sendUserCommand(cmdSetLevel(m_session ? m_session->civAddress() : 0xA4,
                                level::kSquelch, on ? percentToRaw(level) : 0));
}

void IcomCivBackend::setRitEnabled(bool on)
{
    sendUserCommand(cmdRitEnable(m_session ? m_session->civAddress() : 0xA4, on));
}

void IcomCivBackend::setXitEnabled(bool on)
{
    sendUserCommand(cmdXitEnable(m_session ? m_session->civAddress() : 0xA4, on));
}

void IcomCivBackend::setRitOffset(int hz)
{
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
