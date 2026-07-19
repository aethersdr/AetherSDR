#include "SliceModel.h"
#include "core/DigitalVoiceModeRegistry.h"
#include "core/KiwiSdrProtocol.h"
#include <QDebug>

namespace AetherSDR {

// Note: antenna-list splitting now lives in FlexBackend::decodeSliceStatus
// (aetherd RFC 2.3); SliceModel receives the already-split QStringList.

SliceModel::SliceModel(int id, QObject* parent)
    : QObject(parent), m_id(id)
{
    m_lockedFeedbackTimer.setSingleShot(true);
    m_lockedFeedbackTimer.setInterval(kLockedFeedbackMs);
    connect(&m_lockedFeedbackTimer, &QTimer::timeout,
            this, [this] { setLockedFeedbackActive(false); });
}

SliceModel::~SliceModel()
{
    DigitalVoiceModeRegistry::instance().releaseSlice(m_id);
}

void SliceModel::setLockedFeedbackActive(bool on)
{
    if (m_lockedFeedbackActive == on) return;
    m_lockedFeedbackActive = on;
    emit lockedFeedbackActiveChanged(on);
}

// ─── Setters ──────────────────────────────────────────────────────────────────

// Helper: emit commandReady to send the command immediately (when connected),
// or queue it for when the connection becomes available.
void SliceModel::sendCommand(const QString& cmd)
{
    emit commandReady(cmd);
}

// ── Filter polarity (#3434) ─────────────────────────────────────────────
// FlexLib knows FDV only as a USB-family mode (Slice.cs:543-546), so the
// radio reports BOTH FDVU and FDVL passbands in USB form (positive lo/hi).
// Client-side, lower-sideband modes store negative offsets from the carrier
// (the overlay, hit-testing and preset math all assume it). These helpers
// hold the single mode→family mapping every polarity decision uses.
bool SliceModel::filterPolarityUsbFamily(const QString& mode)
{
    return mode == "USB" || mode == "DIGU"
        || mode == "FDV" || mode == "FDVU"   // FlexLib USB-family (FDV incl.)
        || mode == "NT";                     // NAVTEX: USB-family digital (v4.2.18)
}

bool SliceModel::filterPolarityLsbFamily(const QString& mode)
{
    return mode == "LSB" || mode == "DIGL" || mode == "FDVL";
}

bool SliceModel::normalizeFilterPolarity()
{
    // Mirror across the carrier, preserving BOTH edges (asymmetric-safe):
    // (lo,hi) → (-hi,-lo). For symmetric SSB this matches the historical
    // flip (0,2700 → -2700,0); for asymmetric FDVL it keeps the low cut
    // (95,2000 → -2000,-95) instead of collapsing it to (-2000,0), which is
    // the discarded-edge regression #3092 worked around by excluding FDV.
    // Sign-guarded and idempotent: values already in canonical form (and
    // carrier-straddling passbands) are left untouched.
    const bool usbFam = filterPolarityUsbFamily(m_mode);
    const bool lsbFam = filterPolarityLsbFamily(m_mode);
    if ((usbFam && m_filterLow < 0 && m_filterHigh <= 0)
        || (lsbFam && m_filterLow >= 0 && m_filterHigh > 0)) {
        const int lo = m_filterLow, hi = m_filterHigh;
        m_filterLow  = -hi;
        m_filterHigh = -lo;
        return true;
    }
    return false;
}

void SliceModel::setFrequency(double mhz)
{
    if (m_locked) {
        notifyTuneBlockedByLock();
        return;
    }
    if (qFuzzyCompare(m_frequency, mhz)) return;
    m_frequency = mhz;
    // autopan=0 prevents the radio from recentering the pan (#292).
    // SmartSDR pcap confirms: scroll-wheel uses "slice tune <id> <freq> autopan=0".
    sendCommand(QString("slice tune %1 %2 autopan=0").arg(m_id).arg(mhz, 0, 'f', 6));
    emit frequencyChanged(mhz);
}

void SliceModel::tuneAndRecenter(double mhz)
{
    if (m_locked) {
        notifyTuneBlockedByLock();
        return;
    }
    if (qFuzzyCompare(m_frequency, mhz)) return;
    m_frequency = mhz;
    // Without autopan=0, the radio recenters the pan on the new frequency.
    // Used for band changes where recentering is desired.
    sendCommand(QString("slice tune %1 %2").arg(m_id).arg(mhz, 0, 'f', 6));
    emit frequencyChanged(mhz);
}

void SliceModel::setMode(const QString& mode)
{
    const std::optional<DigitalVoiceModeId> requestedMode =
        DigitalVoiceModeRegistry::modeForRadioMode(mode);
    if (m_mode == mode) {
        if (requestedMode.has_value()) {
            const DigitalVoiceModeDescriptor& descriptor =
                DigitalVoiceModeRegistry::descriptor(requestedMode.value());
            const QString previousMode = m_modeBeforeDigitalVoice.isEmpty()
                ? descriptor.underlyingMode
                : m_modeBeforeDigitalVoice;
            QString error;
            if (!DigitalVoiceModeRegistry::instance().claimSlice(
                    requestedMode.value(), m_id, previousMode, &error)) {
                qWarning().noquote()
                    << "SliceModel: restored digital-voice mode rejected:" << error;
            }
        }
        return;
    }

    if (requestedMode.has_value()) {
        const QString previousMode =
            DigitalVoiceModeRegistry::modeForRadioMode(m_mode).has_value()
            ? DigitalVoiceModeRegistry::descriptor(requestedMode.value()).underlyingMode
            : m_mode;
        std::optional<DigitalVoiceSliceClaim> displaced;
        QString error;
        if (!DigitalVoiceModeRegistry::instance().transferSlice(
                requestedMode.value(), m_id, previousMode, &displaced, &error)) {
            qWarning().noquote() << "SliceModel: digital-voice mode rejected:" << error;
            return;
        }
        if (displaced.has_value()) {
            emit digitalVoiceSliceDisplaced(
                displaced->sliceId, displaced->previousMode);
        }
        m_modeBeforeDigitalVoice = previousMode;
    } else if (DigitalVoiceModeRegistry::modeForRadioMode(m_mode).has_value()) {
        DigitalVoiceModeRegistry::instance().releaseSlice(m_id);
        m_modeBeforeDigitalVoice.clear();
    }

    m_mode = mode;
    // aetherd RFC 2.3: express intent; FlexBackend builds "slice set N mode=…"
    // and routes it through the TX-inhibit-guarded slice sink.
    emit modeChangeRequested(mode);
    emit modeChanged(mode);
}

void SliceModel::setFilterWidth(int low, int high)
{
    m_filterLow  = low;
    m_filterHigh = high;
    // Boundary defense (#3434): client-side callers can replay values captured
    // under the pre-mirror convention (band-stack bookmarks, band snapshots,
    // FilterPresets_* in AppSettings, net presets stored positive for FDVL) or
    // pass audio-domain positives (EQ cutoff drag). Normalizing here keeps the
    // model canonical even when the radio's filter already matches and no
    // status echo will arrive to heal it. Sign-guarded: canonical input is
    // untouched.
    normalizeFilterPolarity();
    low = m_filterLow;
    high = m_filterHigh;
    // Operator-driven filter change (preset/drag): bump the user epoch so the
    // adaptive engine adopts this as its new baseline. applyAdaptiveFilter()
    // deliberately does NOT bump it. RFC #3878.
    ++m_userFilterEpoch;
    // FlexAPI: "filt <id> <low_hz> <high_hz>"
    sendCommand(QString("filt %1 %2 %3").arg(m_id).arg(low).arg(high));
    emit filterChanged(low, high);
}

// ── Adaptive RX filter (RFC #3878) ──────────────────────────────────────
// Client-side only: the radio does not store these toggles/bounds, so they
// send no command (cf. setQsk). The engine drives the actual passband via
// applyAdaptiveFilter(); the filter edges themselves remain radio-authoritative.

void SliceModel::setAdaptiveFilterEnabled(bool on)
{
    if (m_adaptiveFilterEnabled == on) return;
    m_adaptiveFilterEnabled = on;
    // Disabling drops any live-fit indication; the engine restores the
    // operator's selected filter separately.
    if (!on) setAdaptiveActive(false);
    emit adaptiveFilterEnabledChanged(on);
}

void SliceModel::setAdaptiveMinLowCut(int hz)
{
    if (m_adaptiveMinLowCut == hz) return;
    m_adaptiveMinLowCut = hz;
    emit adaptiveMinLowCutChanged(hz);
}

void SliceModel::setAdaptiveMaxHighCut(int hz)
{
    if (m_adaptiveMaxHighCut == hz) return;
    m_adaptiveMaxHighCut = hz;
    emit adaptiveMaxHighCutChanged(hz);
}

void SliceModel::setAdaptiveMinSnr(int level)
{
    level = std::clamp(level, 0, 2);
    if (m_adaptiveMinSnr == level) return;
    m_adaptiveMinSnr = level;
    emit adaptiveMinSnrChanged(level);
}

void SliceModel::setAdaptiveResponse(int level)
{
    level = std::clamp(level, 0, 2);
    if (m_adaptiveResponse == level) return;
    m_adaptiveResponse = level;
    emit adaptiveResponseChanged(level);
}

void SliceModel::setAdaptiveSplatter(int level)
{
    level = std::clamp(level, 0, 2);
    if (m_adaptiveSplatter == level) return;
    m_adaptiveSplatter = level;
    emit adaptiveSplatterChanged(level);
}

void SliceModel::setAdaptiveHetReject(bool on)
{
    if (m_adaptiveHetReject == on) return;
    m_adaptiveHetReject = on;
    emit adaptiveHetRejectChanged(on);
}

void SliceModel::setAdaptiveActive(bool on)
{
    if (m_adaptiveActive == on) return;
    m_adaptiveActive = on;
    emit adaptiveActiveChanged(on);
}

void SliceModel::applyAdaptiveFilter(int low, int high)
{
    // Identical wire effect to setFilterWidth() — the radio stays
    // authoritative and we never persist the edges. Kept as a separate entry
    // point so the engine can distinguish its own writes from a user's
    // preset/drag for baseline tracking.
    m_filterLow  = low;
    m_filterHigh = high;
    sendCommand(QString("filt %1 %2 %3").arg(m_id).arg(low).arg(high));
    emit filterChanged(low, high);
}

void SliceModel::setRxAntenna(const QString& ant)
{
    if (m_rxAntenna == ant) return;
    m_rxAntenna = ant;
    sendCommand(QString("slice set %1 rxant=%2").arg(m_id).arg(ant));
    emit rxAntennaChanged(ant);
}

void SliceModel::setTxAntenna(const QString& ant)
{
    if (m_txAntenna == ant) return;
    m_txAntenna = ant;
    sendCommand(QString("slice set %1 txant=%2").arg(m_id).arg(ant));
    emit txAntennaChanged(ant);
}

void SliceModel::setLocked(bool locked)
{
    m_locked = locked;
    // FlexAPI: "slice lock <id>" / "slice unlock <id>"
    sendCommand(locked ? QString("slice lock %1").arg(m_id)
                       : QString("slice unlock %1").arg(m_id));
    if (!locked) {
        m_lockedFeedbackTimer.stop();
        setLockedFeedbackActive(false);
    }
    emit lockedChanged(locked);
}

void SliceModel::notifyTuneBlockedByLock()
{
    if (!m_locked) return;
    emit tuneBlockedByLock();
    // Sustained 500ms gate so every consumer (VFO, RX applet, future
    // status-bar / spectrum / hardware LED) repaints from one source.
    setLockedFeedbackActive(true);
    m_lockedFeedbackTimer.start();
}

void SliceModel::setQsk(bool on)
{
    // QSK is read-only on the slice — controlled via CW applet break_in.
    // This setter exists for model consistency but sends no command.
    if (m_qsk == on) return;
    m_qsk = on;
    emit qskChanged(on);
}

void SliceModel::setNb(bool on)
{
    m_nb = on;
    sendCommand(QString("slice set %1 nb=%2").arg(m_id).arg(on ? 1 : 0));
    emit nbChanged(on);
}

void SliceModel::setNr(bool on)
{
    m_nr = on;
    sendCommand(QString("slice set %1 nr=%2").arg(m_id).arg(on ? 1 : 0));
    emit nrChanged(on);
}

void SliceModel::setAnf(bool on)
{
    m_anf = on;
    sendCommand(QString("slice set %1 anf=%2").arg(m_id).arg(on ? 1 : 0));
    emit anfChanged(on);
}

// v4 DSP toggles — command keys differ from status keys (FlexLib Slice.cs)
void SliceModel::setNrl(bool on)
{
    m_nrl = on;
    sendCommand(QString("slice set %1 lms_nr=%2").arg(m_id).arg(on ? 1 : 0));
    emit nrlChanged(on);
}

void SliceModel::setNrs(bool on)
{
    m_nrs = on;
    sendCommand(QString("slice set %1 speex_nr=%2").arg(m_id).arg(on ? 1 : 0));
    emit nrsChanged(on);
}

void SliceModel::setRnn(bool on)
{
    m_rnn = on;
    sendCommand(QString("slice set %1 rnnoise=%2").arg(m_id).arg(on ? 1 : 0));
    emit rnnChanged(on);
}

void SliceModel::setNrf(bool on)
{
    m_nrf = on;
    sendCommand(QString("slice set %1 nrf=%2").arg(m_id).arg(on ? 1 : 0));
    emit nrfChanged(on);
}

void SliceModel::setAnfl(bool on)
{
    m_anfl = on;
    sendCommand(QString("slice set %1 lms_anf=%2").arg(m_id).arg(on ? 1 : 0));
    emit anflChanged(on);
}

void SliceModel::setAnft(bool on)
{
    m_anft = on;
    sendCommand(QString("slice set %1 anft=%2").arg(m_id).arg(on ? 1 : 0));
    emit anftChanged(on);
}

void SliceModel::setApf(bool on)
{
    m_apf = on;
    sendCommand(QString("slice set %1 apf=%2").arg(m_id).arg(on ? 1 : 0));
    emit apfChanged(on);
}

void SliceModel::setApfLevel(int v)
{
    v = std::clamp(v, 0, 100);
    if (m_apfLevel == v) return;
    m_apfLevel = v;
    sendCommand(QString("slice set %1 apf_level=%2").arg(m_id).arg(v));
    emit apfLevelChanged(v);
}

void SliceModel::setNbLevel(int v)
{
    v = std::clamp(v, 0, 100);
    if (m_nbLevel == v) return;
    m_nbLevel = v;
    sendCommand(QString("slice set %1 nb_level=%2").arg(m_id).arg(v));
    emit nbLevelChanged(v);
}

void SliceModel::setNrLevel(int v)
{
    v = std::clamp(v, 0, 100);
    if (m_nrLevel == v) return;
    m_nrLevel = v;
    sendCommand(QString("slice set %1 nr_level=%2").arg(m_id).arg(v));
    emit nrLevelChanged(v);
}

void SliceModel::setAnfLevel(int v)
{
    v = std::clamp(v, 0, 100);
    if (m_anfLevel == v) return;
    m_anfLevel = v;
    sendCommand(QString("slice set %1 anf_level=%2").arg(m_id).arg(v));
    emit anfLevelChanged(v);
}

void SliceModel::setNrlLevel(int v)
{
    v = std::clamp(v, 0, 100);
    if (m_nrlLevel == v) return;
    m_nrlLevel = v;
    sendCommand(QString("slice set %1 lms_nr_level=%2").arg(m_id).arg(v));
    emit nrlLevelChanged(v);
}

void SliceModel::setNrsLevel(int v)
{
    v = std::clamp(v, 0, 100);
    // Record any explicit user choice (including a deliberate 50) so the
    // applyChanges() re-push won't fight a value the user picked themselves.
    m_nrsLevelUser = v;
    m_nrsLevelUserOverride = true;
    if (m_nrsLevel == v) return;
    m_nrsLevel = v;
    sendCommand(QString("slice set %1 speex_nr_level=%2").arg(m_id).arg(v));
    emit nrsLevelChanged(v);
}

void SliceModel::setNrfLevel(int v)
{
    v = std::clamp(v, 0, 100);
    if (m_nrfLevel == v) return;
    m_nrfLevel = v;
    sendCommand(QString("slice set %1 nrf_level=%2").arg(m_id).arg(v));
    emit nrfLevelChanged(v);
}

void SliceModel::setAnflLevel(int v)
{
    v = std::clamp(v, 0, 100);
    if (m_anflLevel == v) return;
    m_anflLevel = v;
    sendCommand(QString("slice set %1 lms_anf_level=%2").arg(m_id).arg(v));
    emit anflLevelChanged(v);
}

void SliceModel::setAgcMode(const QString& mode)
{
    if (m_externalReceiveAudioReplacement) {
        if (m_externalReceiveAgcMode == mode) {
            return;
        }
        m_externalReceiveAgcMode = mode;
        emit externalReceiveAgcModeChanged(m_externalReceiveAgcMode);
        return;
    }

    if (m_agcMode == mode) {
        return;
    }
    m_agcMode = mode;
    sendCommand(QString("slice set %1 agc_mode=%2").arg(m_id).arg(mode));
    emit agcModeChanged(mode);
}

void SliceModel::setAgcThreshold(int value)
{
    if (m_externalReceiveAudioReplacement) {
        value = qBound(KiwiSdrProtocol::kAgcThresholdMinDb, value,
                       KiwiSdrProtocol::kAgcThresholdMaxDb);
        if (m_externalReceiveAgcThreshold == value) {
            return;
        }
        m_externalReceiveAgcThreshold = value;
        emit externalReceiveAgcThresholdChanged(m_externalReceiveAgcThreshold);
        return;
    }

    value = qBound(0, value, 100);
    if (m_agcThreshold == value) {
        return;
    }
    m_agcThreshold = value;
    sendCommand(QString("slice set %1 agc_threshold=%2").arg(m_id).arg(value));
    emit agcThresholdChanged(value);
}

void SliceModel::setAgcOffLevel(int value)
{
    value = qBound(0, value, 100);
    if (m_externalReceiveAudioReplacement) {
        if (m_externalReceiveAgcOffLevel == value) {
            return;
        }
        m_externalReceiveAgcOffLevel = value;
        emit externalReceiveAgcOffLevelChanged(m_externalReceiveAgcOffLevel);
        return;
    }

    if (m_agcOffLevel == value) {
        return;
    }
    m_agcOffLevel = value;
    sendCommand(QString("slice set %1 agc_off_level=%2").arg(m_id).arg(value));
    emit agcOffLevelChanged(value);
}

void SliceModel::setSquelch(bool on, int level)
{
    if (m_externalReceiveAudioReplacement) {
        level = qBound(0, level, 99);
        const bool onChanged = (m_externalReceiveSquelchOn != on);
        const bool levelChanged = (m_externalReceiveSquelchLevel != level);
        if (!onChanged && !levelChanged) {
            return;
        }
        m_externalReceiveSquelchOn = on;
        m_externalReceiveSquelchLevel = level;
        emit externalReceiveSquelchChanged(on, level);
        return;
    }

    level = qBound(0, level, 100);
    const bool onChanged = (m_squelchOn != on);
    const bool levelChanged = (m_squelchLevel != level);

    m_squelchOn    = on;
    m_squelchLevel = level;

    // FlexLib sends these as separate radio commands. Some firmware/mode
    // combinations reject the combined form even though each field is valid.
    if (onChanged)
        sendCommand(QString("slice set %1 squelch=%2").arg(m_id).arg(on ? 1 : 0));
    if (levelChanged)
        sendCommand(QString("slice set %1 squelch_level=%2").arg(m_id).arg(level));

    emit squelchChanged(on, level);
}

void SliceModel::setExternalReceiveAutoSquelch(bool on)
{
    if (m_externalReceiveAutoSquelch == on) {
        return;
    }
    m_externalReceiveAutoSquelch = on;
    emit externalReceiveAutoSquelchChanged(on);
}

void SliceModel::setRit(bool on, int hz)
{
    m_ritOn   = on;
    m_ritFreq = hz;
    sendCommand(QString("slice set %1 rit_on=%2 rit_freq=%3")
                    .arg(m_id).arg(on ? 1 : 0).arg(hz));
    emit ritChanged(on, hz);
}

void SliceModel::setXit(bool on, int hz)
{
    m_xitOn   = on;
    m_xitFreq = hz;
    sendCommand(QString("slice set %1 xit_on=%2 xit_freq=%3")
                    .arg(m_id).arg(on ? 1 : 0).arg(hz));
    emit xitChanged(on, hz);
}

void SliceModel::setDaxChannel(int ch)
{
    ch = std::clamp(ch, 0, 8);
    if (m_daxChannel == ch) return;
    m_daxChannel = ch;
    sendCommand(QString("slice set %1 dax=%2").arg(m_id).arg(ch));
    emit daxChannelChanged(ch);
}

void SliceModel::setRttyMark(int hz)
{
    if (m_rttyMark == hz) return;
    // Track explicit user override so applyChanges() won't fight an intentional
    // choice of 2125 when rtty_mark_default is non-standard.
    m_rttyMarkUserOverride = (hz == 2125 && m_rttyMarkDefault != 2125);
    m_rttyMark = hz;
    sendCommand(QString("slice set %1 rtty_mark=%2").arg(m_id).arg(hz));
    emit rttyMarkChanged(hz);
}

void SliceModel::setRttyShift(int hz)
{
    if (m_rttyShift == hz) return;
    m_rttyShift = hz;
    sendCommand(QString("slice set %1 rtty_shift=%2").arg(m_id).arg(hz));
    emit rttyShiftChanged(hz);
}

void SliceModel::setDiglOffset(int hz)
{
    if (m_diglOffset == hz) return;
    m_diglOffset = hz;
    sendCommand(QString("slice set %1 digl_offset=%2").arg(m_id).arg(hz));
    emit diglOffsetChanged(hz);
}

void SliceModel::setDiguOffset(int hz)
{
    if (m_diguOffset == hz) return;
    m_diguOffset = hz;
    sendCommand(QString("slice set %1 digu_offset=%2").arg(m_id).arg(hz));
    emit diguOffsetChanged(hz);
}

void SliceModel::setTxSlice(bool on)
{
    sendCommand(QString("slice set %1 tx=%2").arg(m_id).arg(on ? 1 : 0));
}

void SliceModel::setActive(bool on)
{
    if (on) {
        // Optimistic (#3854 review): activeSlice() prefers the radio's active
        // flag, so waiting for the echo leaves a one-round-trip window where
        // the PREVIOUS slice still reads active and the first wheel/MIDI/
        // shortcut inputs land on it (worst over SmartLink latency). The echo
        // remains authoritative — a rejected select is corrected by status.
        if (!m_active) {
            m_active = true;
            emit activeChanged(true);
        }
        sendCommand(QString("slice set %1 active=1").arg(m_id));
    }
}

// ─── Record/playback ────────────────────────────────────────────────────────

void SliceModel::setRecordOn(bool on)
{
    sendCommand(QString("slice set %1 record=%2").arg(m_id).arg(on ? 1 : 0));
}

void SliceModel::setPlayOn(bool on)
{
    sendCommand(QString("slice set %1 play=%2").arg(m_id).arg(on ? 1 : 0));
}

// ─── FM duplex/repeater setters ──────────────────────────────────────────────

void SliceModel::setFmToneMode(const QString& mode)
{
    if (m_fmToneMode == mode) return;
    m_fmToneMode = mode;
    sendCommand(QString("slice set %1 fm_tone_mode=%2").arg(m_id).arg(mode));
    emit fmToneModeChanged(mode);
}

void SliceModel::setFmToneValue(const QString& value)
{
    if (m_fmToneValue == value) return;
    m_fmToneValue = value;
    sendCommand(QString("slice set %1 fm_tone_value=%2").arg(m_id).arg(value));
    emit fmToneValueChanged(value);
}

void SliceModel::setRepeaterOffsetDir(const QString& dir)
{
    if (m_repeaterOffsetDir == dir) return;
    m_repeaterOffsetDir = dir;
    sendCommand(QString("slice set %1 repeater_offset_dir=%2").arg(m_id).arg(dir));
    emit repeaterOffsetDirChanged(dir);
}

void SliceModel::setFmRepeaterOffsetFreq(double mhz)
{
    if (qFuzzyCompare(m_fmRepeaterOffsetFreq, mhz)) return;
    m_fmRepeaterOffsetFreq = mhz;
    sendCommand(QString("slice set %1 fm_repeater_offset_freq=%2")
                    .arg(m_id).arg(mhz, 0, 'f', 6));
    emit fmRepeaterOffsetFreqChanged(mhz);
}

void SliceModel::setTxOffsetFreq(double mhz)
{
    if (qFuzzyCompare(m_txOffsetFreq, mhz)) return;
    m_txOffsetFreq = mhz;
    sendCommand(QString("slice set %1 tx_offset_freq=%2")
                    .arg(m_id).arg(mhz, 0, 'f', 6));
    emit txOffsetFreqChanged(mhz);
}

void SliceModel::setFmDeviation(int hz)
{
    if (m_fmDeviation == hz) return;
    m_fmDeviation = hz;
    sendCommand(QString("slice set %1 fm_deviation=%2").arg(m_id).arg(hz));
    emit fmDeviationChanged(hz);
}

void SliceModel::setAudioGain(float gain)
{
    gain = qBound(0.0f, gain, 100.0f);
    if (m_externalReceiveAudioReplacement) {
        if (m_externalReceiveAudioGain == gain) {
            return;
        }
        m_externalReceiveAudioGain = gain;
        emit audioGainChanged(m_externalReceiveAudioGain);
        return;
    }

    if (m_audioGain == gain) return;
    m_audioGain = gain;
    emit commandReady(QString("slice set %1 audio_level=%2")
        .arg(m_id).arg(static_cast<int>(gain)));
    emit audioGainChanged(m_audioGain);
}

void SliceModel::setRfGain(float gain)
{
    m_rfGain = gain;
    sendCommand(QString("slice set %1 rfgain=%2").arg(m_id).arg(static_cast<int>(gain)));
}

void SliceModel::setAudioMute(bool mute)
{
    const bool previousVisibleMute = audioMute();
    if (m_externalReceiveAudioReplacement) {
        if (m_externalReceiveAudioMute == mute) {
            return;
        }
        m_externalReceiveAudioMute = mute;
        if (audioMute() != previousVisibleMute) {
            emit audioMuteChanged(audioMute());
        }
        return;
    }

    if (m_audioMute == mute) return;
    m_audioMute = mute;
    sendCommand(QString("slice set %1 audio_mute=%2").arg(m_id).arg(mute ? 1 : 0));
    if (audioMute() != previousVisibleMute) {
        emit audioMuteChanged(audioMute());
    }
}

void SliceModel::setExternalReceiveAudioReplacementMute(bool active,
                                                        bool restoreMute)
{
    const bool previousVisibleMute = audioMute();
    const float previousVisibleGain = audioGain();
    const int previousVisiblePan = audioPan();
    const QString previousReceiveAgcMode = receiveAgcMode();
    const int previousReceiveAgcThreshold = receiveAgcThreshold();
    const int previousReceiveAgcOffLevel = receiveAgcOffLevel();
    const bool previousReceiveSquelchOn = receiveSquelchOn();
    const int previousReceiveSquelchLevel = receiveSquelchLevel();
    const bool previousExternalAutoSquelch = m_externalReceiveAutoSquelch;
    if (active) {
        // Only snapshot the Flex gain/pan on the false→true transition. Calling
        // this again while replacement is already active (e.g. switching from
        // one Kiwi RX source to another) must not clobber the external volume
        // the user has since adjusted — see #4300.
        if (!m_externalReceiveAudioReplacement) {
            m_externalReceiveAudioGain = m_audioGain;
            m_externalReceiveAudioPan = m_audioPan;
            m_externalReceiveAudioMute = false;
            m_externalReceiveAudioReplacement = true;
        }
        if (!m_audioMute) {
            m_audioMute = true;
            sendCommand(QString("slice set %1 audio_mute=1").arg(m_id));
        }
    } else {
        m_externalReceiveAudioReplacement = false;
        m_externalReceiveAutoSquelch = false;
        if (m_audioMute != restoreMute) {
            m_audioMute = restoreMute;
            sendCommand(QString("slice set %1 audio_mute=%2")
                            .arg(m_id)
                            .arg(restoreMute ? 1 : 0));
        }
    }
    if (audioMute() != previousVisibleMute) {
        emit audioMuteChanged(audioMute());
    }
    if (audioGain() != previousVisibleGain) {
        emit audioGainChanged(audioGain());
    }
    if (audioPan() != previousVisiblePan) {
        emit audioPanChanged(audioPan());
    }
    if (receiveAgcMode() != previousReceiveAgcMode) {
        if (m_externalReceiveAudioReplacement) {
            emit externalReceiveAgcModeChanged(receiveAgcMode());
        } else {
            emit agcModeChanged(agcMode());
        }
    }
    if (receiveAgcThreshold() != previousReceiveAgcThreshold) {
        if (m_externalReceiveAudioReplacement) {
            emit externalReceiveAgcThresholdChanged(receiveAgcThreshold());
        } else {
            emit agcThresholdChanged(agcThreshold());
        }
    }
    if (receiveAgcOffLevel() != previousReceiveAgcOffLevel) {
        if (m_externalReceiveAudioReplacement) {
            emit externalReceiveAgcOffLevelChanged(receiveAgcOffLevel());
        } else {
            emit agcOffLevelChanged(agcOffLevel());
        }
    }
    if (receiveSquelchOn() != previousReceiveSquelchOn
        || receiveSquelchLevel() != previousReceiveSquelchLevel) {
        if (m_externalReceiveAudioReplacement) {
            emit externalReceiveSquelchChanged(receiveSquelchOn(),
                                               receiveSquelchLevel());
        } else {
            emit squelchChanged(squelchOn(), squelchLevel());
        }
    }
    if (m_externalReceiveAutoSquelch != previousExternalAutoSquelch) {
        emit externalReceiveAutoSquelchChanged(m_externalReceiveAutoSquelch);
    }
}

void SliceModel::setDiversity(bool on)
{
    if (m_diversity == on) return;
    m_diversity = on;
    sendCommand(QString("slice set %1 diversity=%2").arg(m_id).arg(on ? 1 : 0));
    emit diversityChanged(on);
}

void SliceModel::setEscEnabled(bool on)
{
    if (m_escEnabled == on) return;
    m_escEnabled = on;
    // FlexLib: only diversity parent sends ESC commands (Slice.cs:3367)
    // SmartSDR pcap: uses "on"/"off" not "1"/"0"
    if (!m_diversityChild)
        sendCommand(QString("slice set %1 esc=%2").arg(m_id).arg(on ? "on" : "off"));
    emit escEnabledChanged(on);
}

void SliceModel::setEscGain(float gain)
{
    gain = std::clamp(gain, 0.0f, 2.0f);
    if (qFuzzyCompare(m_escGain, gain)) return;
    m_escGain = gain;
    if (!m_diversityChild)
        sendCommand(QString("slice set %1 esc_gain=%2").arg(m_id).arg(gain, 0, 'f', 6));
    emit escGainChanged(gain);
}

void SliceModel::setEscPhaseShift(float deg)
{
    if (qFuzzyCompare(m_escPhaseShift, deg)) return;
    m_escPhaseShift = deg;
    if (!m_diversityChild)
        sendCommand(QString("slice set %1 esc_phase_shift=%2").arg(m_id).arg(deg, 0, 'f', 6));
    emit escPhaseShiftChanged(deg);
}

void SliceModel::setAudioPan(int pan)
{
    pan = qBound(0, pan, 100);
    if (m_externalReceiveAudioReplacement) {
        if (m_externalReceiveAudioPan == pan) {
            return;
        }
        m_externalReceiveAudioPan = pan;
        emit audioPanChanged(m_externalReceiveAudioPan);
        return;
    }

    if (m_audioPan == pan) return;
    m_audioPan = pan;
    sendCommand(QString("slice set %1 audio_pan=%2").arg(m_id).arg(pan));
    emit audioPanChanged(pan);
}

// ─── Status updates from radio ────────────────────────────────────────────────

void SliceModel::emitLetterRefresh()
{
    emit letterChanged(letter());
}

void SliceModel::applyChanges(const SliceDelta& d)
{
    // aetherd RFC 2.3: the Flex slice-status wire decode moved to
    // FlexBackend::decodeSliceStatus, which emits sliceChanged(sliceId, changes)
    // with normalized, canonically-named typed values. This applies those
    // canonical keys — no SmartSDR key names or "1"/string parsing remain here;
    // only the model's business logic (filter-polarity normalization, the
    // override re-pushes, change-gating, emit ordering) stays. Present-only:
    // each key is applied iff the wire reported it.
    bool freqChanged   = false;
    bool modeChanged_  = false;
    bool filterChanged_= false;

    // Panadapter assignment
    if (d.panId.has_value()) {
        const QString p = *d.panId;
        if (m_panId != p) {
            m_panId = p;
            emit panIdChanged(m_panId);
        }
    }

    // Per-client display letter (Multi-Flex assigns independently of sliceId).
    if (d.letter.has_value()) {
        const QString newLetter = *d.letter;
        if (newLetter != m_letter) {
            m_letter = newLetter;
            emit letterChanged(letter());
        }
    }

    if (d.frequency.has_value()) {
        const double f = *d.frequency;
        // qFuzzyCompare fails when either value is 0.0 — use explicit epsilon
        if (std::abs(m_frequency - f) > 1e-9) {
            m_frequency = f;
            freqChanged = true;
            // Band change clears any user override so rtty_mark_default is
            // restored if the radio resets the mark in the same status update.
            m_rttyMarkUserOverride = false;
        }
    }
    if (d.mode.has_value()) {
        const QString m = *d.mode;
        const std::optional<DigitalVoiceModeId> previousMode =
            DigitalVoiceModeRegistry::modeForRadioMode(m_mode);
        const std::optional<DigitalVoiceModeId> incomingMode =
            DigitalVoiceModeRegistry::modeForRadioMode(m);
        bool acceptIncomingMode = true;
        if (incomingMode.has_value()) {
            const DigitalVoiceModeDescriptor& descriptor =
                DigitalVoiceModeRegistry::descriptor(incomingMode.value());
            const QString restoreMode = !m_modeBeforeDigitalVoice.isEmpty()
                ? m_modeBeforeDigitalVoice
                : (previousMode.has_value() ? descriptor.underlyingMode : m_mode);
            QString error;
            if (!DigitalVoiceModeRegistry::instance().claimSlice(
                    incomingMode.value(), m_id, restoreMode, &error)) {
                qWarning().noquote()
                    << "SliceModel: radio reported conflicting digital-voice slice:"
                    << error;
                acceptIncomingMode = false;
                const QString correctedMode = previousMode.has_value()
                    ? restoreMode
                    : m_mode;
                if (m_mode != correctedMode) {
                    m_mode = correctedMode;
                    modeChanged_ = true;
                }
                m_modeBeforeDigitalVoice.clear();
                emit modeChangeRequested(correctedMode);
            } else if (!previousMode.has_value()) {
                m_modeBeforeDigitalVoice = restoreMode;
            }
        } else if (previousMode.has_value()) {
            DigitalVoiceModeRegistry::instance().releaseSlice(m_id);
            m_modeBeforeDigitalVoice.clear();
        }

        if (acceptIncomingMode && m_mode != m) {
            m_mode = m;
            modeChanged_ = true;
        }
    }
    if (d.filterLow.has_value() && d.filterHigh.has_value()) {
        // Full pair: adopt the wire values, then normalize polarity. The radio
        // sometimes reports wrong-polarity offsets after session restore
        // (negative for USB/DIGU), and it ALWAYS reports FDVL in USB form
        // (positive — FlexLib Slice.cs:543-546 knows FDV only as USB-family)
        // while the client stores lower-sideband modes negative (overlay,
        // hit-testing and preset math: lo=-widthHz, hi=-95). #3092 excluded
        // FDV because the old anchored flip discarded one asymmetric edge;
        // normalizeFilterPolarity()'s mirror preserves both edges (#3434).
        m_filterLow  = *d.filterLow;
        m_filterHigh = *d.filterHigh;
        normalizeFilterPolarity();
        filterChanged_ = true;
    } else if (d.filterLow.has_value() || d.filterHigh.has_value()) {
        // One edge without the other. The stored form can legitimately differ
        // from the wire form (FDVL: stored negative, wire positive), and under
        // the mirror a single wire edge maps to the OPPOSITE stored edge
        // (wire filter_hi=3000 for FDVL means stored filterLow=-3000). Merging
        // a wrong-form edge directly would build a carrier-straddling passband
        // that neither pair guard can fix (#3434 review). Sign-gate per edge:
        // wrong-form → crosswise with negation; canonical-form → direct.
        // Applying this rule to both edges of a pair reproduces the pair
        // mirror exactly, so it is a strict generalization.
        const bool haveLo = d.filterLow.has_value();
        const int v = haveLo ? *d.filterLow : *d.filterHigh;
        const bool wrongForm =
            (filterPolarityLsbFamily(m_mode) && v > 0)
            || (filterPolarityUsbFamily(m_mode) && v < 0);
        if (wrongForm) {
            if (haveLo) {
                m_filterHigh = -v;
            } else {
                m_filterLow = -v;
            }
        } else {
            if (haveLo) {
                m_filterLow = v;
            } else {
                m_filterHigh = v;
            }
        }
        filterChanged_ = true;
    } else if (modeChanged_) {
        // Mode changed with NO filter keys in the same delta (a second client
        // flipping FDVU→FDVL mid-session — the reporter's MultiFlex setup).
        // The stored polarity may now be wrong for the new mode; the mirror is
        // sign-guarded and idempotent, so re-running it is safe and fixes the
        // stale-side overlay without waiting for the next filter echo (#3434).
        if (normalizeFilterPolarity()) {
            filterChanged_ = true;
        }
    }
    if (d.modeList.has_value()) {
        const QStringList modes = *d.modeList;
        if (modes != m_modeList) {
            m_modeList = modes;
            emit modeListChanged(modes);
        }
    }
    if (d.active.has_value()) {
        bool a = *d.active;
        if (a != m_active) {
            m_active = a;
            emit activeChanged(a);
        }
    }
    if (d.txSlice.has_value()) {
        bool tx = *d.txSlice;
        if (tx != m_txSlice) {
            m_txSlice = tx;
            emit txSliceChanged(tx);
        }
    }
    if (d.rfGain.has_value()) {
        float g = float(*d.rfGain);
        if (m_rfGain != g) { m_rfGain = g; emit rfGainChanged(g); }
    }
    if (d.audioGain.has_value()) {
        float g = float(*d.audioGain);
        if (m_audioGain != g) {
            const float previousVisibleGain = audioGain();
            m_audioGain = g;
            if (audioGain() != previousVisibleGain) {
                emit audioGainChanged(audioGain());
            }
        }
    }
    if (d.audioPan.has_value()) {
        const int previousVisiblePan = audioPan();
        m_audioPan = *d.audioPan;
        if (audioPan() != previousVisiblePan) {
            emit audioPanChanged(audioPan());
        }
    }
    if (d.audioMute.has_value()) {
        bool mute = *d.audioMute;
        if (mute != m_audioMute) {
            const bool previousVisibleMute = audioMute();
            m_audioMute = mute;
            if (m_externalReceiveAudioReplacement && !m_audioMute) {
                m_audioMute = true;
                sendCommand(QString("slice set %1 audio_mute=1").arg(m_id));
            }
            if (audioMute() != previousVisibleMute) {
                emit audioMuteChanged(audioMute());
            }
        }
    } else if (d.inUse.value_or(false) && m_audioMute) {
        // Full status w/o audio_mute key → radio reset to default (0)
        // on (re)connect. Resync so UI doesn't show a stale 🔇 while
        // audio is actually playing. Radio does not persist audio_mute
        // (see MainWindow.cpp migration note ~line 1264).
        if (m_externalReceiveAudioReplacement) {
            sendCommand(QString("slice set %1 audio_mute=1").arg(m_id));
        } else {
            const bool previousVisibleMute = audioMute();
            m_audioMute = false;
            if (audioMute() != previousVisibleMute) {
                emit audioMuteChanged(audioMute());
            }
        }
    }
    // Parse child/parent flags before emitting diversityChanged so handlers
    // can check isDiversityChild() to gate ESC panel visibility.
    const bool previousDiversityChild = m_diversityChild;
    const bool previousDiversityParent = m_diversityParent;
    const bool previousDiversity = m_diversity;
    const int previousDiversityIndex = m_diversityIndex;
    if (d.diversityChild.has_value()) {
        m_diversityChild = *d.diversityChild;
    }
    if (d.diversityParent.has_value()) {
        m_diversityParent = *d.diversityParent;
    }
    if (d.diversity.has_value()) {
        m_diversity = *d.diversity;
    }
    if (d.diversityIndex.has_value()) {
        m_diversityIndex = *d.diversityIndex;
    }
    if (m_diversityChild != previousDiversityChild
        || m_diversityParent != previousDiversityParent
        || m_diversity != previousDiversity
        || m_diversityIndex != previousDiversityIndex) {
        emit diversityChanged(m_diversity);
    }

    // ESC (Enhanced Signal Clarity) — diversity beamforming ("1"/"on" → bool
    // is normalized in the backend decode).
    if (d.esc.has_value()) {
        bool on = *d.esc;
        if (on != m_escEnabled) { m_escEnabled = on; emit escEnabledChanged(on); }
    }
    if (d.escGain.has_value()) {
        float g = float(*d.escGain);
        if (!qFuzzyCompare(m_escGain, g)) { m_escGain = g; emit escGainChanged(g); }
    }
    if (d.escPhaseShift.has_value()) {
        float p = float(*d.escPhaseShift);
        if (!qFuzzyCompare(m_escPhaseShift, p)) { m_escPhaseShift = p; emit escPhaseShiftChanged(p); }
    }

    // Slice control state (antenna lists are split+trimmed in the backend)
    if (d.rxAntennaList.has_value()) {
        const QStringList ants = *d.rxAntennaList;
        if (ants != m_rxAntennaList) {
            m_rxAntennaList = ants;
            emit rxAntennaListChanged(m_rxAntennaList);
        }
    }
    if (d.txAntennaList.has_value()) {
        const QStringList ants = *d.txAntennaList;
        if (ants != m_txAntennaList) {
            m_txAntennaList = ants;
            emit txAntennaListChanged(m_txAntennaList);
        }
    }
    if (d.rxAntenna.has_value()) {
        m_rxAntenna = *d.rxAntenna;
        emit rxAntennaChanged(m_rxAntenna);
    }
    if (d.txAntenna.has_value()) {
        m_txAntenna = *d.txAntenna;
        emit txAntennaChanged(m_txAntenna);
    }
    if (d.locked.has_value()) {
        m_locked = *d.locked;
        if (!m_locked) {
            m_lockedFeedbackTimer.stop();
            setLockedFeedbackActive(false);
        }
        emit lockedChanged(m_locked);
    }
    if (d.qsk.has_value()) {
        m_qsk = *d.qsk;
        emit qskChanged(m_qsk);
    }
    if (d.nb.has_value()) {
        m_nb = *d.nb;
        emit nbChanged(m_nb);
    }
    if (d.nr.has_value()) {
        m_nr = *d.nr;
        emit nrChanged(m_nr);
    }
    if (d.anf.has_value()) {
        m_anf = *d.anf;
        emit anfChanged(m_anf);
    }
    if (d.nrl.has_value()) {
        m_nrl = *d.nrl;
        emit nrlChanged(m_nrl);
    }
    if (d.nrs.has_value()) {
        m_nrs = *d.nrs;
        emit nrsChanged(m_nrs);
    }
    if (d.rnn.has_value()) {
        m_rnn = *d.rnn;
        emit rnnChanged(m_rnn);
    }
    if (d.nrf.has_value()) {
        m_nrf = *d.nrf;
        emit nrfChanged(m_nrf);
    }
    if (d.anfl.has_value()) {
        m_anfl = *d.anfl;
        emit anflChanged(m_anfl);
    }
    if (d.anft.has_value()) {
        m_anft = *d.anft;
        emit anftChanged(m_anft);
    }
    if (d.apf.has_value()) {
        bool v = *d.apf;
        if (m_apf != v) { m_apf = v; emit apfChanged(v); }
    }
    if (d.apfLevel.has_value()) {
        int v = *d.apfLevel;
        if (m_apfLevel != v) { m_apfLevel = v; emit apfLevelChanged(v); }
    }
    // DSP levels
    if (d.nbLevel.has_value()) {
        int v = *d.nbLevel;
        if (m_nbLevel != v) { m_nbLevel = v; emit nbLevelChanged(v); }
    }
    if (d.nrLevel.has_value()) {
        int v = *d.nrLevel;
        if (m_nrLevel != v) { m_nrLevel = v; emit nrLevelChanged(v); }
    }
    if (d.anfLevel.has_value()) {
        int v = *d.anfLevel;
        if (m_anfLevel != v) { m_anfLevel = v; emit anfLevelChanged(v); }
    }
    if (d.nrlLevel.has_value()) {
        int v = *d.nrlLevel;
        if (m_nrlLevel != v) { m_nrlLevel = v; emit nrlLevelChanged(v); }
    }
    if (d.nrsLevel.has_value()) {
        int v = *d.nrsLevel;
        // The radio's `profile global` snapshot does not persist
        // speex_nr_level. On recall the firmware reports its default of 50
        // even when the user previously set a different value. If we have a
        // cached user choice that differs, push it back. Same precedent as
        // the rtty_mark workaround below.
        if (v == 50 && m_nrsLevelUserOverride && m_nrsLevelUser != 50) {
            v = m_nrsLevelUser;
            sendCommand(QString("slice set %1 speex_nr_level=%2").arg(m_id).arg(v));
        }
        if (m_nrsLevel != v) { m_nrsLevel = v; emit nrsLevelChanged(v); }
    }
    if (d.nrfLevel.has_value()) {
        int v = *d.nrfLevel;
        if (m_nrfLevel != v) { m_nrfLevel = v; emit nrfLevelChanged(v); }
    }
    if (d.anflLevel.has_value()) {
        int v = *d.anflLevel;
        if (m_anflLevel != v) { m_anflLevel = v; emit anflLevelChanged(v); }
    }
    if (d.agcMode.has_value()) {
        m_agcMode = *d.agcMode;
        emit agcModeChanged(m_agcMode);
    }
    if (d.agcThreshold.has_value()) {
        m_agcThreshold = *d.agcThreshold;
        emit agcThresholdChanged(m_agcThreshold);
    }
    if (d.agcOffLevel.has_value()) {
        m_agcOffLevel = *d.agcOffLevel;
        emit agcOffLevelChanged(m_agcOffLevel);
    }
    if (d.squelchOn.has_value() || d.squelchLevel.has_value()) {
        if (d.squelchOn.has_value())
            m_squelchOn = *d.squelchOn;
        if (d.squelchLevel.has_value())
            m_squelchLevel = *d.squelchLevel;
        emit squelchChanged(m_squelchOn, m_squelchLevel);
    }
    if (d.ritOn.has_value() || d.ritFreq.has_value()) {
        if (d.ritOn.has_value())   m_ritOn   = *d.ritOn;
        if (d.ritFreq.has_value()) m_ritFreq = *d.ritFreq;
        emit ritChanged(m_ritOn, m_ritFreq);
    }
    if (d.xitOn.has_value() || d.xitFreq.has_value()) {
        if (d.xitOn.has_value())   m_xitOn   = *d.xitOn;
        if (d.xitFreq.has_value()) m_xitFreq = *d.xitFreq;
        emit xitChanged(m_xitOn, m_xitFreq);
    }
    if (d.daxChannel.has_value()) {
        int ch = *d.daxChannel;
        if (m_daxChannel != ch) { m_daxChannel = ch; emit daxChannelChanged(ch); }
    }
    if (d.rttyMark.has_value()) {
        int v = *d.rttyMark;
        // The radio resets rtty_mark to 2125 on band changes regardless of the
        // configured rtty_mark_default. If we know the default differs and the
        // user has not explicitly chosen 2125, push the default back.
        if (v == 2125 && m_rttyMarkDefault != 2125 && !m_rttyMarkUserOverride) {
            v = m_rttyMarkDefault;
            sendCommand(QString("slice set %1 rtty_mark=%2").arg(m_id).arg(v));
        }
        if (m_rttyMark != v) { m_rttyMark = v; emit rttyMarkChanged(v); }
    }
    if (d.rttyShift.has_value()) {
        int v = *d.rttyShift;
        if (m_rttyShift != v) { m_rttyShift = v; emit rttyShiftChanged(v); }
    }
    if (d.diglOffset.has_value()) {
        int v = *d.diglOffset;
        if (m_diglOffset != v) { m_diglOffset = v; emit diglOffsetChanged(v); }
    }
    if (d.diguOffset.has_value()) {
        int v = *d.diguOffset;
        if (m_diguOffset != v) { m_diguOffset = v; emit diguOffsetChanged(v); }
    }

    // Record/playback status
    if (d.recordOn.has_value()) {
        bool on = *d.recordOn;
        if (m_recordOn != on) { m_recordOn = on; emit recordOnChanged(on); }
    }
    if (d.play.has_value()) {
        const QString v = *d.play;
        if (v == "disabled") {
            if (m_playEnabled) { m_playEnabled = false; emit playEnabledChanged(false); }
            if (m_playOn) { m_playOn = false; emit playOnChanged(false); }
        } else {
            if (!m_playEnabled) { m_playEnabled = true; emit playEnabledChanged(true); }
            bool on = (v == "1");
            if (m_playOn != on) { m_playOn = on; emit playOnChanged(on); }
        }
    }

    // FM duplex/repeater status (lowercase normalization done in the backend)
    if (d.fmToneMode.has_value()) {
        m_fmToneMode = *d.fmToneMode;
        emit fmToneModeChanged(m_fmToneMode);
    }
    if (d.fmToneValue.has_value()) {
        double v = *d.fmToneValue;
        m_fmToneValue = QString::number(v, 'f', 1);
        emit fmToneValueChanged(m_fmToneValue);
    }
    if (d.repeaterOffsetDir.has_value()) {
        m_repeaterOffsetDir = *d.repeaterOffsetDir;
        emit repeaterOffsetDirChanged(m_repeaterOffsetDir);
    }
    if (d.fmRepeaterOffsetFreq.has_value()) {
        m_fmRepeaterOffsetFreq = *d.fmRepeaterOffsetFreq;
        emit fmRepeaterOffsetFreqChanged(m_fmRepeaterOffsetFreq);
    }
    if (d.txOffsetFreq.has_value()) {
        m_txOffsetFreq = *d.txOffsetFreq;
        emit txOffsetFreqChanged(m_txOffsetFreq);
    }
    if (d.fmDeviation.has_value()) {
        m_fmDeviation = *d.fmDeviation;
        emit fmDeviationChanged(m_fmDeviation);
    }

    if (d.step.has_value() || d.stepList.has_value()) {
        bool changed = false;
        if (d.step.has_value()) {
            int s = *d.step;
            if (s != m_stepHz) { m_stepHz = s; changed = true; }
        }
        if (d.stepList.has_value()) {
            QVector<int> list;
            for (const auto& v : (*d.stepList).split(QLatin1Char(','))) {
                if (v.isEmpty()) continue;
                // Fail closed on a malformed step token: skip it rather than
                // admit a bogus 0-Hz step into the tuning-step list (#4068 review).
                bool ok = false;
                const int n = v.toInt(&ok);
                if (ok) list.append(n);
            }
            if (list != m_stepList) { m_stepList = list; changed = true; }
        }
        if (changed) emit stepChanged(m_stepHz, m_stepList);
    }

    if (freqChanged)
        emit frequencyChanged(m_frequency);
    if (modeChanged_)   emit modeChanged(m_mode);
    if (filterChanged_) emit filterChanged(m_filterLow, m_filterHigh);
}

QStringList SliceModel::drainPendingCommands()
{
    QStringList cmds;
    cmds.swap(m_pendingCommands);
    return cmds;
}

} // namespace AetherSDR
