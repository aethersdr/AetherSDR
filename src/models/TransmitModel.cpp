#include "TransmitModel.h"
#include "core/ClientQuindarTone.h"
#include "core/LogManager.h"
#include <QDebug>
#include <QTimer>
#include <QThread>
#include <utility>

namespace AetherSDR {

TransmitModel::TransmitModel(QObject* parent)
    : QObject(parent)
{}

TransmitModel::~TransmitModel()
{
    invalidatePttRelease();
}

void TransmitModel::resetState()
{
    cancelPttRelease();
    m_apdEnabled = false;
    m_apdConfigurable = false;
    m_apdEqActive = false;
    m_apdSamplers.clear();
    m_rfPower = 100;
    m_tunePower = 10;
    m_tune = false;
    m_mox = false;
    m_transmitting = false;
    m_maxPowerLevel = 100;
    m_atuEnabled = false;
    m_atuStatus = ATUStatus::None;
    m_memoriesEnabled = false;
    m_usingMemory = false;
    m_showTxInWaterfall = false;
    m_txSliceMode.clear();
    setTuneAvailable(true);

    // The committed break-in delay belonged to the session that just ended; drop
    // it so setCwSpeed() does not re-assert a delay the operator never set in
    // this one (#5288 review — cross-session leak). RadioModel::onDisconnected()
    // is the only caller, so this fires on EVERY disconnect — a transient drop
    // and reconnect to the same radio, not just a radio swap.
    //
    // The m_holdBreakInDelay opt-in is a client preference, not radio state, and
    // is left untouched — PhoneCwApplet re-applies it from AppSettings. That
    // asymmetry is deliberate but it leaves the feature on-but-unarmed, so say
    // so out loud: the applet renders that state distinctly rather than showing
    // one checked button for both (#5288 review, blocker 1).
    // Clear BEFORE emitting: the applet's slot reads holdBreakInDelayArmed()
    // synchronously, so emitting first hands it the state we are in the middle
    // of leaving and the button keeps showing "holding 48 ms" after the delay is
    // gone. (Caught driving the GUI, not in review.)
    const bool wasArmed = (m_cwDelayHeld > 0);
    m_cwDelayHeld = -1;   // also covers the 0 (deliberate QSK) case
    if (wasArmed) {
        emit holdBreakInDelayArmedChanged(false);
    }

    emit apdStateChanged();
    emit transmittingChanged(false);
    emit moxChanged(false);
    emit tuneChanged(false);
    emit micStateChanged();
}

// ── Status parsing ──────────────────────────────────────────────────────────

// aetherd RFC 2.3: the five Flex transmit-family status decoders
// (applyTransmitStatus/Interlock/Atu/Apd/ApdSampler) moved to
// FlexBackend::decode*Status, which translate the SmartSDR wire into a typed
// TransmitDelta and emit transmitChanged. This applies the present fields —
// no wire key names or "1"/clamp parsing remain here; only the model's business
// logic (compander/dexp aliasing, the grouped emits, the ATU enum parse, the
// per-antenna sampler map + selected-fallback) stays. Present-only: each field
// is applied iff its optional is engaged.
namespace {
// Present-only change-apply: writes *src into dst iff engaged AND different,
// returning whether it changed. Collapses the ~50 field-apply lines and names
// the emit-flag exactly once per call site (#4071 review). The compander/dexp
// alias, ATU parse, and sampler map stay bespoke below.
template <class T>
bool assign(const std::optional<T>& src, T& dst)
{
    if (src && dst != *src) { dst = *src; return true; }
    return false;
}
}  // namespace

void TransmitModel::applyChanges(const TransmitDelta& d)
{
    bool changed = false;
    bool tuneChanged_ = false;
    bool micChanged = false;
    bool phoneChanged = false;
    bool filterCutoffChanged = false;
    bool cwPitchChanged_ = false;
    bool cwSpeedChanged_ = false;

    // ── Core transmit ──
    // rf_power / tune_power emit inline (like max_power_level below): the
    // radio restores per-band power on QSY, and TCI clients need that edge
    // distinctly, not folded into the catch-all stateChanged() (#4161).
    if (assign(d.rfPower, m_rfPower))   { changed = true; emit rfPowerChanged(m_rfPower); }
    if (assign(d.tunePower, m_tunePower)) { changed = true; emit tunePowerChanged(m_tunePower); }
    if (assign(d.tune, m_tune)) { changed = true; tuneChanged_ = true; }
    // Backend MOX is observed radio state, not this client's transmit intent.
    // RadioModel publishes it on radioTransmittingChanged for presentation
    // consumers.  Routing it through setTransmitting() would emit moxChanged
    // and could open this client's mic/DAX/serial-PTT paths when hardware PTT
    // or another network client keys the radio.
    changed |= assign(d.mox, m_mox);
    changed |= assign(d.transmitFreq, m_transmitFreq);

    // ── Mic / monitor / processor ──
    micChanged |= assign(d.micSelection, m_micSelection);
    micChanged |= assign(d.micLevel, m_micLevel);
    micChanged |= assign(d.micAcc, m_micAcc);
    micChanged |= assign(d.speechProcEnable, m_speechProcEnable);
    if (d.speechProcLevel) {
        const int level = qBound(0, *d.speechProcLevel, m_speechProcLevelMaximum);
        if (m_speechProcLevel != level) {
            m_speechProcLevel = level;
            micChanged = true;
        }
    }
    // compander/dexp are aliased: one wire value drives BOTH member pairs (the
    // compander → mic side and the dexp → phone side). Bespoke — one optional,
    // two members, two flags.
    if (d.compander) {
        const bool v = *d.compander;
        if (m_companderOn != v) { m_companderOn = v; micChanged = true; }
        if (m_dexpOn != v)      { m_dexpOn = v;      phoneChanged = true; }
    }
    if (d.companderLevel) {
        const int v = *d.companderLevel;
        if (m_companderLevel != v) { m_companderLevel = v; micChanged = true; }
        if (m_dexpLevel != v)      { m_dexpLevel = v;      phoneChanged = true; }
    }
    micChanged |= assign(d.dax, m_daxOn);
    micChanged |= assign(d.sbMonitor, m_sbMonitor);
    micChanged |= assign(d.monGainSb, m_monGainSb);

    // ── VOX / phone ──
    phoneChanged |= assign(d.voxEnable, m_voxEnable);
    phoneChanged |= assign(d.voxLevel, m_voxLevel);
    phoneChanged |= assign(d.voxDelay, m_voxDelay);
    phoneChanged |= assign(d.micBoost, m_micBoost);
    phoneChanged |= assign(d.micBias, m_micBias);
    changed      |= assign(d.metInRx, m_metInRx);   // met_in_rx → stateChanged, not phone
    phoneChanged |= assign(d.syncCwx, m_syncCwx);
    phoneChanged |= assign(d.amCarrierLevel, m_amCarrierLevel);
    if (assign(d.txFilterLow, m_txFilterLow))   { phoneChanged = true; filterCutoffChanged = true; }
    if (assign(d.txFilterHigh, m_txFilterHigh)) { phoneChanged = true; filterCutoffChanged = true; }

    // ── CW ──
    if (assign(d.cwSpeed, m_cwSpeed)) { phoneChanged = true; cwSpeedChanged_ = true; }
    if (assign(d.cwPitch, m_cwPitch)) { phoneChanged = true; cwPitchChanged_ = true; }
    phoneChanged |= assign(d.cwBreakIn, m_cwBreakIn);
    // Radio status is authoritative on the live break-in delay: adopt it, full
    // stop. The "hold" protection against SmartSDR's speed-linked QSK-floor
    // walk lives on the operator-intent path (setCwSpeed), never here — a
    // command issued from this status-apply path is the feedback loop
    // Principle II forbids (#5288 review).
    phoneChanged |= assign(d.cwDelay, m_cwDelay);
    phoneChanged |= assign(d.cwSidetone, m_cwSidetone);
    phoneChanged |= assign(d.cwIambic, m_cwIambic);
    phoneChanged |= assign(d.cwIambicMode, m_cwIambicMode);
    phoneChanged |= assign(d.cwSwapPaddles, m_cwSwapPaddles);
    phoneChanged |= assign(d.cwlEnabled, m_cwlEnabled);
    phoneChanged |= assign(d.monGainCw, m_monGainCw);
    phoneChanged |= assign(d.monPanCw, m_monPanCw);

    // ── Misc TX (max_power_level / tx_slice_mode emit inline, like the old code) ──
    if (assign(d.maxPowerLevel, m_maxPowerLevel)) { changed = true; emit maxPowerLevelChanged(m_maxPowerLevel); }
    changed |= assign(d.tuneMode, m_tuneMode);
    changed |= assign(d.showTxInWaterfall, m_showTxInWaterfall);
    if (assign(d.txSliceMode, m_txSliceMode)) { changed = true; emit txSliceModeChanged(m_txSliceMode); }

    // ── Interlock (no emit — plain state, matching applyInterlockStatus) ──
    if (d.accTxDelay)       m_accTxDelay       = *d.accTxDelay;
    if (d.tx1Delay)         m_tx1Delay         = *d.tx1Delay;
    if (d.tx2Delay)         m_tx2Delay         = *d.tx2Delay;
    if (d.tx3Delay)         m_tx3Delay         = *d.tx3Delay;
    if (d.txDelay)          m_txDelay          = *d.txDelay;
    if (d.interlockTimeout) m_interlockTimeout = *d.interlockTimeout;
    if (d.accTxReqPolarity) m_accTxReqPolarity = *d.accTxReqPolarity;
    if (d.rcaTxReqPolarity) m_rcaTxReqPolarity = *d.rcaTxReqPolarity;

    // Core/mic/phone emits (same order the old applyTransmitStatus used).
    if (changed) emit stateChanged();
    if (tuneChanged_) emit tuneChanged(m_tune);
    if (micChanged) emit micStateChanged();
    if (phoneChanged) emit phoneStateChanged();
    if (filterCutoffChanged) emit txFilterCutoffChanged(m_txFilterLow, m_txFilterHigh);
    if (cwPitchChanged_) emit cwPitchChanged(m_cwPitch);
    if (cwSpeedChanged_) emit cwSpeedChanged(m_cwSpeed);

    // ── ATU (own emit; model owns the enum parse) ──
    {
        bool atuChanged = false;
        if (d.atuStatusRaw) {
            const ATUStatus s = parseAtuTuneStatus(*d.atuStatusRaw);
            if (m_atuStatus != s) { m_atuStatus = s; atuChanged = true; }
        }
        atuChanged |= assign(d.atuEnabled, m_atuEnabled);
        atuChanged |= assign(d.memoriesEnabled, m_memoriesEnabled);
        atuChanged |= assign(d.usingMemory, m_usingMemory);
        if (atuChanged) emit atuStateChanged();
    }

    // ── APD (own emit) ──
    {
        bool apdChanged = false;
        apdChanged |= assign(d.apdEnabled, m_apdEnabled);
        apdChanged |= assign(d.apdConfigurable, m_apdConfigurable);
        apdChanged |= assign(d.apdEqActive, m_apdEqActive);
        // Bare equalizer_reset flag: clear active + emit the reset signal.
        if (d.apdEqualizerReset) {
            if (m_apdEqActive) { m_apdEqActive = false; apdChanged = true; }
            emit apdEqualizerResetReceived();
        }
        if (apdChanged) emit apdStateChanged();
    }

    // ── APD sampler (per-TX-antenna map + selected fallback) ──
    if (d.apdSamplerTxAnt) {
        const QString txAnt = *d.apdSamplerTxAnt;
        ApdSampler s = m_apdSamplers.value(txAnt);
        bool samplerChanged = false;
        if (d.apdSamplerAvailable && s.available != *d.apdSamplerAvailable) {
            s.available = *d.apdSamplerAvailable;
            samplerChanged = true;
        }
        if (d.apdSamplerSelected) {
            QString sel = *d.apdSamplerSelected;
            // Fall back to INTERNAL if the selected port isn't available (FlexLib).
            if (!s.available.contains(sel)) sel = QStringLiteral("INTERNAL");
            if (s.selected != sel) { s.selected = sel; samplerChanged = true; }
        }
        if (samplerChanged) {
            m_apdSamplers.insert(txAnt, s);
            emit apdSamplerChanged(txAnt);
        }
    }
}

void TransmitModel::setApdEnabled(bool on)
{
    if (m_apdEnabled != on) {
        m_apdEnabled = on;
        emit apdStateChanged();
    }
    emit commandReady(QString("apd enable=%1").arg(on ? 1 : 0));
}

void TransmitModel::setApdSamplerPort(const QString& txAnt, const QString& port)
{
    if (txAnt.isEmpty() || port.isEmpty()) return;
    emit commandReady(QString("apd sampler tx_ant=%1 sample_port=%2")
                          .arg(txAnt.toUpper(), port.toUpper()));
}

void TransmitModel::resetApdEqualizer()
{
    emit commandReady(QStringLiteral("apd reset"));
}

void TransmitModel::setProfileList(const QStringList& profiles)
{
    if (m_profileList != profiles) {
        m_profileList = profiles;
        emit profileListChanged();
    }
}

void TransmitModel::setActiveProfile(const QString& profile)
{
    if (m_activeProfile != profile) {
        m_activeProfile = profile;
        emit stateChanged();
    }
}

// ── Commands ────────────────────────────────────────────────────────────────

void TransmitModel::setHostModulation(bool on)
{
    if (m_hostModulation == on)
        return;
    m_hostModulation = on;
    if (on) {
        // PC is the only source that exists on a host-modulating backend, so it
        // is asserted rather than defaulted — a stale "MIC" carried over from a
        // Flex session would otherwise sit there transmitting silence.
        m_micInputList = QStringList{QStringLiteral("PC")};
        if (m_micSelection != QLatin1String("PC")) {
            m_micSelection = QStringLiteral("PC");
            emit phoneStateChanged();
        }
        emit micInputListChanged();
    }
    emit hostModulationChanged(on);
}

void TransmitModel::setHasTuner(bool present)
{
    if (m_hasTuner == present)
        return;
    m_hasTuner = present;
    emit hasTunerChanged(present);
}

void TransmitModel::setHasTunerMemories(bool present)
{
    if (m_hasTunerMemories == present) {
        return;
    }
    m_hasTunerMemories = present;
    emit hasTunerMemoriesChanged(present);
}

void TransmitModel::setRfPower(int power)
{
    power = qBound(0, power, 100);
    if (m_rfPower != power) {
        m_rfPower = power;
        emit rfPowerChanged(power);
        emit stateChanged();
    }
    emit commandReady(QString("transmit set rfpower=%1").arg(power));
    emit rfPowerCommandIssued(power);
}

void TransmitModel::setTunePower(int power)
{
    power = qBound(0, power, 100);
    if (m_tunePower != power) {
        m_tunePower = power;
        emit tunePowerChanged(power);
        emit stateChanged();
    }
    emit commandReady(QString("transmit set tunepower=%1").arg(power));
}

void TransmitModel::setTuneMode(const QString& mode)
{
    if (mode != "single_tone" && mode != "two_tone") {
        qWarning() << "TransmitModel: ignoring invalid tune mode:" << mode;
        return;
    }
    emit commandReady("transmit set tune_mode=" + mode);
}

void TransmitModel::setTuneAvailable(bool available)
{
    if (m_tuneAvailable == available) {
        return;
    }
    m_tuneAvailable = available;
    emit tuneAvailabilityChanged(available);
}

void TransmitModel::startTune(PttSource source)
{
    requestTune(source, false, {});
}

void TransmitModel::startTwoToneTune(PttSource source)
{
    requestTune(source, true, {});
}

void TransmitModel::requestTune(PttSource source, bool twoTone, const KeyingRoute& route)
{
    if (!m_tuneAvailable) {
        return;
    }
    if (!runPttPreflight(source, false)) {
        return;
    }
    if (!tuneAdmitted()) {
        return;
    }
    const KeyingPermit permit = route.admit ? route.admit(true)
        : m_keyingAdmission ? m_keyingAdmission(KeyingIntent::Tune, true) : KeyingPermit{};
    if ((route.admit || m_keyingAdmission) && (!permit || !permit())) {
        return;
    }
    const quint64 intentEpoch = ++m_tuneIntentEpoch;

    // Tag the initiating source so the status-bar operator TX timer can exclude
    // local TUNE carriers as well as TCI/DAX-initiated tune (the radio reports
    // every software path as source=SW). Without this, tune inherits the stale
    // Mox tag and wrongly runs the operator-only timer. (#4131 review)
    m_activePttSource = source;
    if (twoTone) {
        setTuneMode(QStringLiteral("two_tone"));
        if (intentEpoch != m_tuneIntentEpoch || (permit && !permit())) {
            return;
        }
    }

    // Optimistic tune state, exactly as setMox() does for m_transmitting.
    //
    // m_tune was previously set ONLY from a radio status delta. Flex reports
    // tune=1 back; a Hermes-Lite 2 reports nothing, so isTuning() stayed false
    // forever and TxApplet's toggle — "if (isTuning()) stopTune() else
    // startTune()" — could never take the stop branch. TUNE latched on and the
    // only way out was keying MOX twice. Radio status still reconciles this on
    // backends that send it.
    if (!m_tune) {
        m_tune = true;
        emit tuneChanged(true);
    }
    if (intentEpoch == m_tuneIntentEpoch && (!permit || permit())) {
        if (route.dispatch) {
            route.dispatch(true);
        } else {
            emit tuneCommandIssued(true);
        }
    }
}

void TransmitModel::toggleTwoToneTune()
{
    if (isTuning()) {
        stopTune();
        // Revert to single_tone after a two-tone shortcut session so the
        // next regular Tune press isn't surprised by sticky two-tone state
        // on the radio.  Tune mode is no longer persisted; selecting "Two
        // Tone" is now a transient one-shot via the TUNE button's right-
        // click menu in TxApplet.
        setTuneMode(QStringLiteral("single_tone"));
    } else {
        startTwoToneTune();
    }
}

void TransmitModel::stopTune()
{
    stopTune({});
}

void TransmitModel::stopTune(const KeyingRoute& route)
{
    const KeyingPermit permit = route.admit ? route.admit(false)
        : m_keyingAdmission ? m_keyingAdmission(KeyingIntent::Tune, false) : KeyingPermit{};
    if (route.admit && (!permit || !permit())) {
        return;
    }
    const quint64 intentEpoch = ++m_tuneIntentEpoch;
    if (m_tune) {
        m_tune = false;
        emit tuneChanged(false);
    }
    if (intentEpoch == m_tuneIntentEpoch && (!permit || permit())) {
        if (route.dispatch) {
            route.dispatch(false);
        } else {
            emit tuneCommandIssued(false);
        }
    }
}

void TransmitModel::setMox(bool on)
{
    if (on && !runPttPreflight(m_activePttSource)) {
        return;
    }
    const KeyingPermit permit = m_keyingAdmission ? m_keyingAdmission(KeyingIntent::Mox, on) : KeyingPermit{};
    if (on && m_keyingAdmission && (!permit || !permit())) {
        return;
    }
    const quint64 intentEpoch = ++m_moxIntentEpoch;
    invalidatePttRelease();
    // Optimistic MOX edge gating keeps UI/audio aligned with user intent.
    // Interlock status from the radio will still reconcile final state.
    if (m_transmitting != on) {
        m_transmitting = on;
        emit transmittingChanged(on);
        if (intentEpoch != m_moxIntentEpoch || m_transmitting != on || (permit && !permit())) {
            return;
        }
        emit moxChanged(on);
    }
    if (intentEpoch == m_moxIntentEpoch && (!permit || permit())) {
        emit moxCommandIssued(on);
    }
}

void TransmitModel::setTransmitting(bool tx)
{
    if (tx == m_transmitting) return;
    m_transmitting = tx;
    emit transmittingChanged(tx);
    if (m_transmitting != tx) {
        return;
    }
    // Keep moxChanged for backward compat — CW decoder gate and QSO recorder
    // currently gate on this signal and need interlock-driven TX edges too.
    emit moxChanged(tx);
}

void TransmitModel::atuStart()
{
    requestAtu(true, {});
}

void TransmitModel::atuBypass()
{
    requestAtu(false, {});
}

void TransmitModel::requestAtu(bool start, const KeyingRoute& route)
{
    const KeyingPermit permit = route.admit ? route.admit(start)
        : start && m_keyingAdmission ? m_keyingAdmission(KeyingIntent::Atu, true) : KeyingPermit{};
    if ((route.admit || (start && m_keyingAdmission)) && (!permit || !permit())) {
        return;
    }
    if (route.dispatch) {
        route.dispatch(start);
    } else {
        emit atuCommandIssued(start);
    }
}

void TransmitModel::setAtuMemories(bool on)
{
    emit commandReady(QString("atu set memories_enabled=%1").arg(on ? 1 : 0));
}

void TransmitModel::atuClearMemories()
{
    // FlexLib Radio.cs:11055-11060 confirms "atu clear" wipes the entire
    // ATU memory database. There is no per-band variant and no status echo;
    // the only visible side effect is that subsequent using_mem=1 flags
    // stop appearing on previously-stored frequencies. (#2624)
    emit commandReady("atu clear");
}

void TransmitModel::loadProfile(const QString& name)
{
    emit commandReady(QString("profile tx load \"%1\"").arg(name));
}

// ── Mic profile setters (called from RadioModel) ────────────────────────────

void TransmitModel::setMicProfileList(const QStringList& profiles)
{
    if (m_micProfileList != profiles) {
        m_micProfileList = profiles;
        emit micProfileListChanged();
    }
}

void TransmitModel::setActiveMicProfile(const QString& profile)
{
    if (m_activeMicProfile != profile) {
        m_activeMicProfile = profile;
        emit micStateChanged();
    }
}

void TransmitModel::setMicInputList(const QStringList& inputs)
{
    if (m_micInputList != inputs) {
        m_micInputList = inputs;
        emit micInputListChanged();
    }
}

// ── Mic / monitor / processor commands ──────────────────────────────────────

void TransmitModel::setMicSelection(const QString& input)
{
    const QString normalized = input.toUpper();
    if (m_micSelection != normalized) {
        m_micSelection = normalized;
        emit micStateChanged();
    }
    emit commandReady(QString("mic input %1").arg(normalized));
}

void TransmitModel::setMicLevel(int level)
{
    level = qBound(0, level, 100);
    if (m_micLevel != level) {
        m_micLevel = level;
        emit micStateChanged();  // PhoneCwApplet's mic slider binds to this
    }
    // Unconditional, like commandReady below and deliberately NOT inside the
    // changed test: a host-modulating backend is the authority on its own gain
    // and may have been reset (reconnect, radio swap) while m_micLevel stood
    // still. Re-asserting a value the seam already holds is free; failing to
    // re-assert one it has lost leaves the operator's slider lying.
    emit micLevelCommandIssued(level);
    emit commandReady(QString("transmit set miclevel=%1").arg(level));
}

void TransmitModel::setMicAcc(bool on)
{
    emit commandReady(QString("mic acc %1").arg(on ? 1 : 0));
}

void TransmitModel::setSpeechProcessorEnable(bool on)
{
    // Pcap confirmed: SmartSDR uses speech_processor_enable (not compander).
    // Optimistic update: radio does not echo speech_processor_enable in
    // incremental status — only in the initial full dump on connect.
    m_speechProcEnable = on;
    emit micStateChanged();
    emit speechProcessorCommandIssued(m_speechProcEnable, m_speechProcLevel);
    emit commandReady(QString("transmit set speech_processor_enable=%1").arg(on ? 1 : 0));
}

void TransmitModel::setSpeechProcessorLevel(int level)
{
    // Flex uses NOR=0, DX=1, DX+=2 (pcap confirmed:
    // speech_processor_level, not compander_level). A backend capability may
    // widen the normalized domain for an evidenced continuous control.
    // Optimistic update: Flex does not echo in incremental status.
    level = qBound(0, level, m_speechProcLevelMaximum);
    m_speechProcLevel = level;
    emit micStateChanged();
    emit speechProcessorCommandIssued(m_speechProcEnable, m_speechProcLevel);
    emit commandReady(QString("transmit set speech_processor_level=%1").arg(level));
}

void TransmitModel::setSpeechProcessorLevelMaximum(int maximum)
{
    maximum = qBound(2, maximum, 100);
    if (m_speechProcLevelMaximum == maximum) {
        return;
    }
    m_speechProcLevelMaximum = maximum;
    const int bounded = qBound(0, m_speechProcLevel, maximum);
    if (bounded != m_speechProcLevel) {
        m_speechProcLevel = bounded;
        emit micStateChanged();
    }
}

bool TransmitModel::applySpeechProcessorState(bool on, int level)
{
    level = qBound(0, level, m_speechProcLevelMaximum);
    if (m_speechProcEnable == on && m_speechProcLevel == level) {
        return false;
    }
    m_speechProcEnable = on;
    m_speechProcLevel = level;
    emit micStateChanged();
    return true;
}

bool TransmitModel::applyMicSelectionState(const QString& input)
{
    if (input.isEmpty() || m_micSelection == input) {
        return false;
    }
    m_micSelection = input;
    emit micStateChanged();
    return true;
}

void TransmitModel::setDax(bool on)
{
    // Optimistic local update mirroring the sibling mic setters; the radio's
    // dax= status echo (parsed above, under the micChanged path) supersedes.
    if (m_daxOn != on) {
        m_daxOn = on;
        emit micStateChanged();  // PhoneCwApplet's DAX button binds to this
    }
    emit commandReady(QString("transmit set dax=%1").arg(on ? 1 : 0));
}

void TransmitModel::setSbMonitor(bool on)
{
    // Optimistic update — radio status echo (sb_monitor) supersedes. micStateChanged
    // is the signal the MON button's model->widget sync (syncPhoneFromModel) binds to,
    // matching the sibling setMonGainSb; the sync is guarded by m_updatingFromModel so
    // the optimistic setChecked cannot re-emit the command.
    if (m_sbMonitor != on) {
        m_sbMonitor = on;
        emit micStateChanged();
    }
    emit commandReady(QString("transmit set mon=%1").arg(on ? 1 : 0));
    emit monitorCommandIssued(m_sbMonitor, m_monGainSb);
}

void TransmitModel::setMonGainSb(int gain)
{
    gain = qBound(0, gain, 100);
    m_monGainSb = gain;
    emit micStateChanged();
    emit commandReady(QString("transmit set mon_gain_sb=%1").arg(gain));
    emit monitorCommandIssued(m_sbMonitor, m_monGainSb);
}

void TransmitModel::loadMicProfile(const QString& name)
{
    emit commandReady(QString("profile mic load \"%1\"").arg(name));
}

// ── VOX commands ────────────────────────────────────────────────────────────

void TransmitModel::setVoxEnable(bool on)
{
    m_voxEnable = on;  // optimistic update — radio may not echo
    emit phoneStateChanged();
    emit commandReady(QString("transmit set vox_enable=%1").arg(on ? 1 : 0));
    emit voxCommandIssued(on, m_voxLevel, m_voxDelay);
}

void TransmitModel::setVoxLevel(int level)
{
    level = qBound(0, level, 100);
    m_voxLevel = level;
    emit phoneStateChanged();
    emit commandReady(QString("transmit set vox_level=%1").arg(level));
    emit voxCommandIssued(m_voxEnable, m_voxLevel, m_voxDelay);
}

void TransmitModel::setVoxDelay(int delay)
{
    delay = qBound(0, delay, 100);
    m_voxDelay = delay;
    emit phoneStateChanged();
    emit commandReady(QString("transmit set vox_delay=%1").arg(delay));
    emit voxCommandIssued(m_voxEnable, m_voxLevel, m_voxDelay);
}

void TransmitModel::setMicBoost(bool on)
{
    m_micBoost = on;  // optimistic — radio sends no status echo (#1045)
    emit phoneStateChanged();
    emit commandReady(QString("mic boost %1").arg(on ? 1 : 0));
}

void TransmitModel::setMicBias(bool on)
{
    m_micBias = on;  // optimistic — radio sends no status echo (#1045)
    emit phoneStateChanged();
    emit commandReady(QString("mic bias %1").arg(on ? 1 : 0));
}

void TransmitModel::setAmCarrierLevel(int level)
{
    level = qBound(0, level, 100);
    if (m_amCarrierLevel != level) {
        m_amCarrierLevel = level;  // optimistic — radio status echo supersedes
        emit phoneStateChanged();
    }
    emit commandReady(QString("transmit set am_carrier=%1").arg(level));
}

void TransmitModel::setDexp(bool on)
{
    // FlexLib v4.2.18 and a SmartSDR v4.2.20 capture show DEXP is the
    // radio's compander control; older dexp/noise_gate keys are rejected.
    m_dexpOn = on;
    m_companderOn = on;
    emit phoneStateChanged();
    emit micStateChanged();
    emit commandReady(QString("transmit set compander=%1").arg(on ? 1 : 0));
}

void TransmitModel::setDexpLevel(int level)
{
    level = qBound(0, level, 100);
    // See setDexp(): SmartSDR backs DEXP level with compander_level.
    m_dexpLevel = level;
    m_companderLevel = level;
    emit phoneStateChanged();
    emit micStateChanged();
    emit commandReady(QString("transmit set compander_level=%1").arg(level));
}

// The TX passband setters all take the same shape: bound, adopt OPTIMISTICALLY,
// announce the intent, and emit the Flex verb.
//
// The optimistic adoption is what makes these work on a radio that modulates on
// this host. A Flex echoes `transmit` status and applyStatus() writes the state
// back, so the local fields could be left alone; a host-modulating backend never
// echoes anything, so without this the operator drags the low-cut slider, the
// verb goes nowhere, no status returns, and the control springs back — while the
// modulator keeps whatever passband its mode default gave it. Same pattern, and
// the same reason, as setSpeechProcessorEnable() above.
//
// txFilterCommandIssued is OPERATOR INTENT only — applyStatus() must never emit
// it — so a backend can bind to it without echoing radio state back as a fresh
// command (Principle II).
void TransmitModel::setTxFilterLow(int hz)
{
    setTxFilter(qBound(kTxFilterMinHz, hz, kTxFilterMaxHz), m_txFilterHigh);
}

void TransmitModel::setTxFilterHigh(int hz)
{
    setTxFilter(m_txFilterLow, qBound(kTxFilterMinHz, hz, kTxFilterMaxHz));
}

void TransmitModel::setTxFilter(int lowHz, int highHz)
{
    lowHz  = qBound(kTxFilterMinHz, lowHz, kTxFilterMaxHz - kTxFilterMinWidthHz);
    highHz = qBound(lowHz + kTxFilterMinWidthHz, highHz, kTxFilterMaxHz);
    if (m_txFilterLow != lowHz || m_txFilterHigh != highHz) {
        m_txFilterLow = lowHz;
        m_txFilterHigh = highHz;
        emit txFilterCutoffChanged(m_txFilterLow, m_txFilterHigh);
        emit phoneStateChanged();
    }
    emit txFilterCommandIssued(lowHz, highHz);
    emit commandReady(QString("transmit set filter_low=%1 filter_high=%2")
                      .arg(lowHz).arg(highHz));
}

// ── CW commands ─────────────────────────────────────────────────────────────

void TransmitModel::setCwSpeed(int wpm)
{
    wpm = qBound(5, wpm, 100);
    const bool speedChanged = (m_cwSpeed != wpm);
    if (speedChanged) {
        m_cwSpeed = wpm;
        emit phoneStateChanged();
        emit cwSpeedChanged(m_cwSpeed);
    }
    emit cwSpeedCommandIssued(wpm);
    emit commandReady(QString("cw wpm %1").arg(wpm));

    // Hold break-in delay (#5288, opt-in): SmartSDR re-pins break_in_delay to a
    // WPM-derived QSK floor on a speed change and walks it down as WPM rises —
    // on an inline amplifier that can't tolerate QSK that is silent
    // hot-switching. This is the operator's own speed command, so re-asserting
    // the delay they set is a request on the command path like any other setter
    // — not the client overriding radio truth (which is why it is here and not
    // in applyChanges). Ride it out right behind the `cw wpm` so the floor never
    // takes lasting effect. If the radio rejects it as below the new floor it
    // keeps its own larger value, the next status echo is adopted normally, and
    // the amp still sees more delay than the operator asked for — safe. A set
    // delay of 0 is deliberate QSK: m_cwDelayHeld is not > 0, nothing fires.
    //
    // Gated on a REAL speed change, not on the setter being called: the floor
    // only moves when the speed does, and the knob paths clamp
    // (MainWindow_Controllers.cpp's WheelCwSpeed, MainWindow_Shortcuts' +/-5)
    // so a detent held against 5 or 100 would otherwise re-send on every tick.
    //
    // Note what this deliberately does NOT do: it does not write m_cwDelay. An
    // earlier revision adopted the held value locally "so the slider does not
    // dip between the two echoes", but it never achieved that — the radio's
    // floor echo still arrives and applyChanges still adopts it, so the dip
    // happens either way. What the local write did do was display a value the
    // radio had refused: the floor is enforced on the write side too
    // (`Parameter out of range`, #5519), and when a speed change does not move
    // the floor the radio sends no break_in_delay status at all, so nothing
    // ever corrected the model. m_cwDelay stays radio truth, full stop
    // (#5288 review, blocker 2).
    if (speedChanged && m_holdBreakInDelay && m_cwDelayHeld > 0) {
        emit commandReady(QString("cw break_in_delay %1").arg(m_cwDelayHeld));
        // Log only the real divergence — the radio's delay having actually moved
        // off what the operator set — not every prophylactic re-send. qCWarning,
        // not qCInfo: aether.transmit is a QtWarningMsg category, so Info would
        // not reach a default support bundle, and a client value held against
        // radio status is exactly the divergence Principle II's rationale says
        // must be logged (#5288 review).
        if (m_cwDelay != m_cwDelayHeld) {
            qCWarning(lcTransmit).nospace()
                << "TransmitModel: break-in delay had moved to " << m_cwDelay
                << " ms; hold re-asserted the operator's " << m_cwDelayHeld
                << " ms after CW speed -> " << wpm << " wpm";
        }
    }
}

void TransmitModel::setCwPitch(int hz)
{
    hz = qBound(100, hz, 6000);
    if (m_cwPitch != hz) {
        m_cwPitch = hz;  // update local cache so rapid steppers accumulate
        emit phoneStateChanged();
        emit cwPitchChanged(hz);
    }
    emit cwPitchCommandIssued(hz);
    emit commandReady(QString("cw pitch %1").arg(hz));
}

void TransmitModel::setCwBreakIn(bool on)
{
    if (m_cwBreakIn != on) {
        m_cwBreakIn = on;
        emit phoneStateChanged();
    }
    emit cwBreakInCommandIssued(on);
    emit commandReady(QString("cw break_in %1").arg(on ? 1 : 0));
}

void TransmitModel::setCwDelay(int ms)
{
    ms = qBound(0, ms, 2000);
    // The operator's explicit word on the break-in delay — the value the "hold"
    // opt-in (#5288) re-asserts after a speed change. Recorded even when hold is
    // off so enabling it later picks up the current delay with no surprise; a
    // deliberate 0 (full QSK) is recorded too and simply never re-asserted.
    const bool wasArmed = (m_cwDelayHeld > 0);
    m_cwDelayHeld = ms;
    if (wasArmed != (m_cwDelayHeld > 0)) {
        emit holdBreakInDelayArmedChanged(m_cwDelayHeld > 0);
    }
    if (m_cwDelay != ms) {
        m_cwDelay = ms;
        emit phoneStateChanged();
    }
    emit commandReady(QString("cw break_in_delay %1").arg(ms));
}

void TransmitModel::setHoldBreakInDelay(bool on)
{
    if (m_holdBreakInDelay == on) {
        return;
    }
    m_holdBreakInDelay = on;
    // Deliberately does NOT seed m_cwDelayHeld — that would capture whatever the
    // radio last reported (or the construction default, if this is the settings
    // restore firing before any radio data) and re-assert a value the operator
    // never chose, the "authoritative but radio-seeded" flaw the first revision
    // had. The hold protects the delay the operator SET (setCwDelay); until
    // they set one this session there is simply nothing to hold.
    emit holdBreakInDelayChanged(on);
}

void TransmitModel::setCwSidetone(bool on)
{
    if (m_cwSidetone != on) {
        m_cwSidetone = on;  // optimistic — radio status echo supersedes
        emit phoneStateChanged();
    }
    emit commandReady(QString("cw sidetone %1").arg(on ? 1 : 0));
}

void TransmitModel::setCwIambic(bool on)
{
    // Optimistic update — radio firmware v1.4.0.0 doesn't echo `iambic`
    // back in subsequent transmit statuses, so without this our local
    // state goes stale after every user toggle.
    if (m_cwIambic != on) {
        m_cwIambic = on;
        emit phoneStateChanged();
    }
    emit commandReady(QString("cw iambic %1").arg(on ? 1 : 0));
}

void TransmitModel::setCwIambicMode(int mode)
{
    mode = qBound(0, mode, 1);
    if (m_cwIambicMode != mode) {
        m_cwIambicMode = mode;
        emit phoneStateChanged();
    }
    emit commandReady(QString("cw mode %1").arg(mode));
}

void TransmitModel::setCwSwapPaddles(bool on)
{
    if (m_cwSwapPaddles != on) {
        m_cwSwapPaddles = on;
        emit phoneStateChanged();
    }
    emit commandReady(QString("cw swap %1").arg(on ? 1 : 0));
}

void TransmitModel::setCwlEnabled(bool on)
{
    if (m_cwlEnabled != on) {
        m_cwlEnabled = on;
        emit phoneStateChanged();
    }
    emit commandReady(QString("cw cwl_enabled %1").arg(on ? 1 : 0));
}

void TransmitModel::setMonGainCw(int gain)
{
    gain = qBound(0, gain, 100);
    if (m_monGainCw != gain) {
        m_monGainCw = gain;
        emit phoneStateChanged();
    }
    emit commandReady(QString("transmit set mon_gain_cw=%1").arg(gain));
}

void TransmitModel::setMonPanCw(int pan)
{
    pan = qBound(0, pan, 100);
    if (m_monPanCw != pan) {
        m_monPanCw = pan;
        emit phoneStateChanged();
    }
    emit commandReady(QString("transmit set mon_pan_cw=%1").arg(pan));
}

// ── Helpers ─────────────────────────────────────────────────────────────────

ATUStatus TransmitModel::parseAtuTuneStatus(const QString& s)
{
    // Values from FlexLib Radio.cs ParseATUTuneStatus()
    if (s == "NONE")               return ATUStatus::None;
    if (s == "TUNE_NOT_STARTED")   return ATUStatus::NotStarted;
    if (s == "TUNE_IN_PROGRESS")   return ATUStatus::InProgress;
    if (s == "TUNE_BYPASS")        return ATUStatus::Bypass;
    if (s == "TUNE_SUCCESSFUL")    return ATUStatus::Successful;
    if (s == "TUNE_OK")            return ATUStatus::OK;
    if (s == "TUNE_FAIL_BYPASS")   return ATUStatus::FailBypass;
    if (s == "TUNE_FAIL")          return ATUStatus::Fail;
    if (s == "TUNE_ABORTED")       return ATUStatus::Aborted;
    if (s == "TUNE_MANUAL_BYPASS") return ATUStatus::ManualBypass;
    qCDebug(lcTransmit) << "TransmitModel: unknown ATU status:" << s;
    return ATUStatus::None;
}

// ─────────────────────────────────────────────────────────────────────
// PTT request coordinator (#2262 — Quindar tones)
// ─────────────────────────────────────────────────────────────────────

void TransmitModel::setQuindarTone(ClientQuindarTone* tone)
{
    m_quindarTone = tone;
}

void TransmitModel::setTxModeGetter(TxModeGetter getter)
{
    m_txModeGetter = std::move(getter);
}

void TransmitModel::setPttPreflight(PttPreflight preflight)
{
    m_pttPreflight = std::move(preflight);
}

void TransmitModel::setTuneAdmission(TuneAdmission admission)
{
    m_tuneAdmission = std::move(admission);
}

bool TransmitModel::tuneAdmitted()
{
    if (m_tune) {
        return true;   // already tuning: a repeated start is not a new admission
    }
    if (!m_tuneAdmission) {
        return true;
    }
    const QString message = m_tuneAdmission().trimmed();
    if (message.isEmpty()) {
        return true;
    }
    emit pttBlocked(message);
    emit tuneChanged(m_tune);   // a TUNE toggle may have flipped before calling
    return false;
}

void TransmitModel::setPttOffHook(PttOffHook hook)
{
    m_pttOffHook = std::move(hook);
}

void TransmitModel::clearPttOffHook()
{
    m_pttOffHook = nullptr;
}

bool TransmitModel::isPhoneModeForQuindar() const
{
    if (!m_txModeGetter) return false;
    const QString m = m_txModeGetter();
    // Phone modes accepted for Quindar: SSB families, AM, FM.
    // Digital modes intentionally excluded — the tone would corrupt the
    // digital waveform. FreeDV (FDV/FDVU/FDVL) is excluded for the same
    // reason: it now uses RADAE (the same neural encoder as RADE mode),
    // so a Quindar sine produces codec-artifact noise on air rather than
    // a recognisable signalling tone.
    return m == "USB" || m == "LSB"
        || m == "AM"  || m == "FM"  || m == "NFM";
}

bool TransmitModel::runPttPreflight(PttSource source, bool resyncMoxOnBlock)
{
    if (!m_pttPreflight)
        return true;

    const QString message = m_pttPreflight(source).trimmed();
    if (message.isEmpty())
        return true;

    cancelPendingQuindarOff();
    emit pttBlocked(message);

    // A checked MOX button has already toggled before requestPttOn() runs.
    // Force a UI resync even when the internal state was already RX.
    if (resyncMoxOnBlock) {
        if (m_transmitting)
            setTransmitting(false);
        else
            emit moxChanged(false);
    }
    return false;
}

void TransmitModel::cancelPendingQuindarOff()
{
    if (m_pendingMoxOffTimer) {
        m_pendingMoxOffTimer->stop();
        m_pendingMoxOffTimer->deleteLater();
        m_pendingMoxOffTimer = nullptr;
    }
    m_quindarOutroInFlight = false;
}

void TransmitModel::invalidatePttRelease()
{
    const std::function<void()> abandoned = std::exchange(m_pttReleaseAbandoned, {});
    if (m_pttReleaseFence) {
        m_pttReleaseFence->store(false, std::memory_order_release);
        m_pttReleaseFence.reset();
    }
    if (abandoned) {
        abandoned();
    }
}

void TransmitModel::cancelPttRelease()
{
    const quint64 intentEpoch = ++m_moxIntentEpoch;
    ++m_tuneIntentEpoch;
    invalidatePttRelease();
    cancelPendingQuindarOff();
    if (m_quindarTone) {
        m_quindarTone->forceIdle();
    }
    emit quindarActiveChanged(false);
    if (intentEpoch == m_moxIntentEpoch) {
        emit pttReleaseCancelled();
    }
}

TransmitModel::PttRelease TransmitModel::capturePttRelease(PttRelease release)
{
    invalidatePttRelease();
    m_pttReleaseFence = std::make_shared<std::atomic<bool>>(true);
    const std::shared_ptr<std::atomic<bool>> fence = m_pttReleaseFence;
    m_pttReleaseAbandoned = std::move(release.abandoned);
    return {[fence, current = std::move(release.isCurrent)] {
                return fence->load(std::memory_order_acquire) && (!current || current());
            },
            [this, fence, finish = std::move(release.finish), ownerThread = thread()] {
                if (QThread::currentThread() == ownerThread) {
                    if (!fence->exchange(false, std::memory_order_acq_rel)) {
                        return;
                    }
                    // Normal completion owns its queued unkey. Cancellation
                    // must not retire that producer before the write returns.
                    m_pttReleaseAbandoned = {};
                    if (finish) {
                        finish();
                    } else {
                        setMox(false);
                    }
                } else {
                    qCWarning(lcProtocol) << "PTT release refused off the model owning thread";
                }
            }, {}};
}

void TransmitModel::dispatchMoxOff(const PttRelease& release)
{
    if (!release.current()) {
        return;
    }
    if (m_pttOffHook) {
        m_pttOffHook(release);
        return;
    }
    release.release();
}

void TransmitModel::requestPttOn(PttSource source)
{
    requestPttOn(source, {}, {});
}

void TransmitModel::requestPttOn(PttSource source, std::function<KeyingPermit()> admit,
                                std::function<void()> engage)
{
    if (!runPttPreflight(source)) {
        return;
    }
    const KeyingPermit permit = admit ? admit()
        : m_keyingAdmission ? m_keyingAdmission(KeyingIntent::Mox, true) : KeyingPermit{};
    if ((admit || m_keyingAdmission) && (!permit || !permit())) {
        return;
    }
    invalidatePttRelease();

    // Remember who asked to key so the status-bar TX timer can exclude
    // TCI-hardware and DAX transmits (both surface as source=SW at the radio).
    m_activePttSource = source;

    // If Quindar is enabled + phone mode + we have an engine, start
    // the intro tone alongside MOX so the radio keys up while the
    // tone plays (the tone gets transmitted as part of the audio).
    auto* tone = m_quindarTone;

    // Coalesce a re-engage that fires during the outro window — flip
    // phase back to Live, cancel the pending xmit-0 timer, and skip a
    // fresh intro so the user doesn't feel an outro+intro dead zone.
    if (tone && tone->isEnabled()
        && tone->phase() == ClientQuindarTone::Phase::Disengaging) {
        if (tone->coalesceReEngage()) {
            cancelPendingQuindarOff();
            // Outro flash ends — phase is now back in Live, no tone
            // playing locally.  MOX is already true (we never sent
            // xmit 0); just bail.
            emit quindarActiveChanged(false);
            if (engage && (!permit || permit())) {
                engage(); // transfer the backend fence to this admitted producer
            }
            return;
        }
    }

    if (source != PttSource::Wspr
        && tone && tone->isEnabled() && isPhoneModeForQuindar()) {
        tone->startIntro();
        // Flash the QUIN chip for the intro duration; the audio thread
        // auto-transitions Engaging → Live when its frame counter
        // hits the same duration, so we model the visible flash with
        // a single-shot timer here on the GUI thread.
        emit quindarActiveChanged(true);
        const int introMs = std::max(50, tone->currentIntroDurationMs());
        QTimer::singleShot(introMs, this, [this]() {
            emit quindarActiveChanged(false);
        });
    }
    if (!permit || permit()) {
        if (engage) {
            engage();
        } else {
            setMox(true);
        }
    }
}

void TransmitModel::requestPttOff(PttSource source)
{
    requestPttOff(source, {});
}

void TransmitModel::requestPttOff(PttSource /*source*/, PttRelease scopedRelease)
{
    if (m_pttReleaseFence && m_pttReleaseFence->load(std::memory_order_acquire)) {
        return; // a duplicate release must not truncate an in-flight normal tail
    }
    const PttRelease release = capturePttRelease(std::move(scopedRelease));
    auto* tone = m_quindarTone;

    // No Quindar, no phone mode, or already shutting down → straight
    // through.  The phase check is essential — if MOX was never on
    // (or already off) we shouldn't run an outro.
    if (!tone || !tone->isEnabled() || !isPhoneModeForQuindar()
        || tone->phase() == ClientQuindarTone::Phase::Idle
        || m_quindarOutroInFlight) {
        cancelPendingQuindarOff();
        dispatchMoxOff(release);
        return;
    }

    // Start the outro and defer xmit 0 by the outro duration so the
    // tone gets transmitted before the radio unkeys.  Outro duration
    // is style-dependent and computed from current settings.
    tone->startOutro();
    emit quindarActiveChanged(true);
    if (!release.current()) {
        return;
    }
    const int outroMs = std::max(50, tone->currentOutroDurationMs());

    cancelPendingQuindarOff();
    m_quindarOutroInFlight = true;
    m_pendingMoxOffTimer = new QTimer(this);
    m_pendingMoxOffTimer->setSingleShot(true);
    m_pendingMoxOffTimer->setInterval(outroMs);
    connect(m_pendingMoxOffTimer, &QTimer::timeout, this, [this, release, timer = m_pendingMoxOffTimer]() {
        // If a re-engage happened during the outro window the timer
        // would have been cancelled; if we're here, the outro fully
        // completed and it's safe to flip MOX off.
        timer->deleteLater();
        if (timer != m_pendingMoxOffTimer) {
            return;
        }
        m_pendingMoxOffTimer = nullptr;
        m_quindarOutroInFlight = false;
        emit quindarActiveChanged(false);
        dispatchMoxOff(release);
    });
    m_pendingMoxOffTimer->start();
}

} // namespace AetherSDR
