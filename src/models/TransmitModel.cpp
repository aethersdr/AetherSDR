#include "TransmitModel.h"
#include "core/ClientQuindarTone.h"
#include "core/LogManager.h"
#include <QDebug>
#include <QTimer>

namespace AetherSDR {

TransmitModel::TransmitModel(QObject* parent)
    : QObject(parent)
{}

void TransmitModel::resetState()
{
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

    // ── Core transmit ──
    changed |= assign(d.rfPower, m_rfPower);
    changed |= assign(d.tunePower, m_tunePower);
    if (assign(d.tune, m_tune)) { changed = true; tuneChanged_ = true; }
    changed |= assign(d.mox, m_mox);
    changed |= assign(d.transmitFreq, m_transmitFreq);

    // ── Mic / monitor / processor ──
    micChanged |= assign(d.micSelection, m_micSelection);
    micChanged |= assign(d.micLevel, m_micLevel);
    micChanged |= assign(d.micAcc, m_micAcc);
    micChanged |= assign(d.speechProcEnable, m_speechProcEnable);
    micChanged |= assign(d.speechProcLevel, m_speechProcLevel);
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
    phoneChanged |= assign(d.cwSpeed, m_cwSpeed);
    phoneChanged |= assign(d.cwPitch, m_cwPitch);
    phoneChanged |= assign(d.cwBreakIn, m_cwBreakIn);
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

void TransmitModel::setRfPower(int power)
{
    power = qBound(0, power, 100);
    if (m_rfPower != power) {
        m_rfPower = power;
        emit stateChanged();
    }
    emit commandReady(QString("transmit set rfpower=%1").arg(power));
}

void TransmitModel::setTunePower(int power)
{
    power = qBound(0, power, 100);
    if (m_tunePower != power) {
        m_tunePower = power;
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

void TransmitModel::startTune(PttSource source)
{
    if (!runPttPreflight(source, false))
        return;

    // Tag the initiating source so the status-bar operator TX timer can exclude
    // a TCI/DAX-initiated tune (the radio reports both as source=SW). An
    // operator tune (source=Tune) is neither TCI nor DAX, so it still shows the
    // timer. Without this, an external-app tune inherits the stale Mox tag and
    // wrongly runs the "operator-only" timer. (#4131 review)
    m_activePttSource = source;

    emit commandReady("transmit tune 1");
}

void TransmitModel::startTwoToneTune(PttSource source)
{
    if (!runPttPreflight(source, false))
        return;

    m_activePttSource = source;   // exclude TCI/DAX-initiated tune (see startTune, #4131)
    setTuneMode("two_tone");
    emit commandReady("transmit tune 1");
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
    emit commandReady("transmit tune 0");
}

void TransmitModel::setMox(bool on)
{
    // Optimistic MOX edge gating keeps UI/audio aligned with user intent.
    // Interlock status from the radio will still reconcile final state.
    if (m_transmitting != on) {
        m_transmitting = on;
        emit transmittingChanged(on);
        emit moxChanged(on);
    }
    emit commandReady(QString("xmit %1").arg(on ? 1 : 0));
}

void TransmitModel::setTransmitting(bool tx)
{
    if (tx == m_transmitting) return;
    m_transmitting = tx;
    emit transmittingChanged(tx);
    // Keep moxChanged for backward compat — CW decoder gate and QSO recorder
    // currently gate on this signal and need interlock-driven TX edges too.
    emit moxChanged(tx);
}

void TransmitModel::atuStart()
{
    emit commandReady("atu start");
}

void TransmitModel::atuBypass()
{
    emit commandReady("atu bypass");
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
    emit commandReady(QString("transmit set speech_processor_enable=%1").arg(on ? 1 : 0));
}

void TransmitModel::setSpeechProcessorLevel(int level)
{
    // NOR=0, DX=1, DX+=2 (pcap confirmed: speech_processor_level, not compander_level).
    // Optimistic update: radio does not echo in incremental status.
    level = qBound(0, level, 2);
    m_speechProcLevel = level;
    emit micStateChanged();
    emit commandReady(QString("transmit set speech_processor_level=%1").arg(level));
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
}

void TransmitModel::setMonGainSb(int gain)
{
    gain = qBound(0, gain, 100);
    m_monGainSb = gain;
    emit micStateChanged();
    emit commandReady(QString("transmit set mon_gain_sb=%1").arg(gain));
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
}

void TransmitModel::setVoxLevel(int level)
{
    level = qBound(0, level, 100);
    m_voxLevel = level;
    emit phoneStateChanged();
    emit commandReady(QString("transmit set vox_level=%1").arg(level));
}

void TransmitModel::setVoxDelay(int delay)
{
    delay = qBound(0, delay, 100);
    m_voxDelay = delay;
    emit phoneStateChanged();
    emit commandReady(QString("transmit set vox_delay=%1").arg(delay));
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

void TransmitModel::setTxFilterLow(int hz)
{
    hz = qBound(0, hz, 10000);
    emit commandReady(QString("transmit set filter_low=%1 filter_high=%2")
                      .arg(hz).arg(m_txFilterHigh));
}

void TransmitModel::setTxFilterHigh(int hz)
{
    hz = qBound(0, hz, 10000);
    emit commandReady(QString("transmit set filter_low=%1 filter_high=%2")
                      .arg(m_txFilterLow).arg(hz));
}

// ── CW commands ─────────────────────────────────────────────────────────────

void TransmitModel::setCwSpeed(int wpm)
{
    wpm = qBound(5, wpm, 100);
    if (m_cwSpeed != wpm) {
        m_cwSpeed = wpm;
        emit phoneStateChanged();
    }
    emit commandReady(QString("cw wpm %1").arg(wpm));
}

void TransmitModel::setCwPitch(int hz)
{
    hz = qBound(100, hz, 6000);
    if (m_cwPitch != hz) {
        m_cwPitch = hz;  // update local cache so rapid steppers accumulate
        emit phoneStateChanged();
    }
    emit commandReady(QString("cw pitch %1").arg(hz));
}

void TransmitModel::setCwBreakIn(bool on)
{
    if (m_cwBreakIn != on) {
        m_cwBreakIn = on;
        emit phoneStateChanged();
    }
    emit commandReady(QString("cw break_in %1").arg(on ? 1 : 0));
}

void TransmitModel::setCwDelay(int ms)
{
    ms = qBound(0, ms, 2000);
    if (m_cwDelay != ms) {
        m_cwDelay = ms;
        emit phoneStateChanged();
    }
    emit commandReady(QString("cw break_in_delay %1").arg(ms));
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
    emit commandReady(QString("cw swap %1").arg(on ? 1 : 0));
}

void TransmitModel::setCwlEnabled(bool on)
{
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

void TransmitModel::dispatchMoxOff()
{
    if (m_pttOffHook) {
        m_pttOffHook();
        return;
    }
    setMox(false);
}

void TransmitModel::requestPttOn(PttSource source)
{
    if (!runPttPreflight(source))
        return;

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
            return;
        }
    }

    if (tone && tone->isEnabled() && isPhoneModeForQuindar()) {
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
    setMox(true);
}

void TransmitModel::requestPttOff(PttSource /*source*/)
{
    auto* tone = m_quindarTone;

    // No Quindar, no phone mode, or already shutting down → straight
    // through.  The phase check is essential — if MOX was never on
    // (or already off) we shouldn't run an outro.
    if (!tone || !tone->isEnabled() || !isPhoneModeForQuindar()
        || tone->phase() == ClientQuindarTone::Phase::Idle
        || m_quindarOutroInFlight) {
        cancelPendingQuindarOff();
        dispatchMoxOff();
        return;
    }

    // Start the outro and defer xmit 0 by the outro duration so the
    // tone gets transmitted before the radio unkeys.  Outro duration
    // is style-dependent and computed from current settings.
    tone->startOutro();
    m_quindarOutroInFlight = true;
    emit quindarActiveChanged(true);
    const int outroMs = std::max(50, tone->currentOutroDurationMs());

    cancelPendingQuindarOff();
    m_pendingMoxOffTimer = new QTimer(this);
    m_pendingMoxOffTimer->setSingleShot(true);
    m_pendingMoxOffTimer->setInterval(outroMs);
    connect(m_pendingMoxOffTimer, &QTimer::timeout, this, [this]() {
        // If a re-engage happened during the outro window the timer
        // would have been cancelled; if we're here, the outro fully
        // completed and it's safe to flip MOX off.
        m_pendingMoxOffTimer = nullptr;
        m_quindarOutroInFlight = false;
        emit quindarActiveChanged(false);
        dispatchMoxOff();
    });
    m_pendingMoxOffTimer->start();
}

} // namespace AetherSDR
