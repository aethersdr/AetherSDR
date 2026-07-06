#include "SliceModel.h"
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
    if (m_mode == mode) return;
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
    // applyStatus() re-push won't fight a value the user picked themselves.
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
    // Track explicit user override so applyStatus() won't fight an intentional
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
    if (on)
        sendCommand(QString("slice set %1 active=1").arg(m_id));
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
        m_externalReceiveAudioGain = m_audioGain;
        m_externalReceiveAudioPan = m_audioPan;
        m_externalReceiveAudioMute = false;
        m_externalReceiveAudioReplacement = true;
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

void SliceModel::applyChanges(const QVariantMap& c)
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
    if (c.contains(QStringLiteral("panId"))) {
        const QString p = c.value(QStringLiteral("panId")).toString();
        if (m_panId != p) {
            m_panId = p;
            emit panIdChanged(m_panId);
        }
    }

    // Per-client display letter (Multi-Flex assigns independently of sliceId).
    if (c.contains(QStringLiteral("letter"))) {
        const QString newLetter = c.value(QStringLiteral("letter")).toString();
        if (newLetter != m_letter) {
            m_letter = newLetter;
            emit letterChanged(letter());
        }
    }

    if (c.contains(QStringLiteral("frequency"))) {
        const double f = c.value(QStringLiteral("frequency")).toDouble();
        // qFuzzyCompare fails when either value is 0.0 — use explicit epsilon
        if (std::abs(m_frequency - f) > 1e-9) {
            m_frequency = f;
            freqChanged = true;
            // Band change clears any user override so rtty_mark_default is
            // restored if the radio resets the mark in the same status update.
            m_rttyMarkUserOverride = false;
        }
    }
    if (c.contains(QStringLiteral("mode"))) {
        const QString m = c.value(QStringLiteral("mode")).toString();
        if (m_mode != m) {
            m_mode = m;
            modeChanged_ = true;
        }
    }
    if (c.contains(QStringLiteral("filterLow")) || c.contains(QStringLiteral("filterHigh"))) {
        // The radio may report one edge without the other; keep the current
        // value for the absent one (the old parse defaulted to the member).
        m_filterLow  = c.contains(QStringLiteral("filterLow"))
            ? c.value(QStringLiteral("filterLow")).toInt() : m_filterLow;
        m_filterHigh = c.contains(QStringLiteral("filterHigh"))
            ? c.value(QStringLiteral("filterHigh")).toInt() : m_filterHigh;

        // Radio sometimes sends wrong-polarity filter offsets after session
        // restore (e.g. negative offsets for USB/DIGU). Normalize based on mode.
        //
        // FDV/FDVU/FDVL are excluded: FreeDV passbands are asymmetric
        // (e.g. 95..widthHz for FDVU; -widthHz..-95 for FDVL — see
        // VfoWidget.cpp:3773-3777) and FlexLib only knows "FDV" as a
        // USB-family mode (Slice.cs:545-550). When the radio echoes an
        // asymmetric FDVL filter as USB-form (positive lo/hi), the
        // anchored flip discards one edge and offsets the overlay (#3092).
        const bool isUsbFamily = (m_mode == "USB" || m_mode == "DIGU"
                                  || m_mode == "NT");  // NAVTEX: USB-family digital (v4.2.18)
        const bool isLsbFamily = (m_mode == "LSB" || m_mode == "DIGL");
        if (isUsbFamily && m_filterLow < 0 && m_filterHigh <= 0) {
            // Flip: -2700,0 → 0,2700
            int w = std::abs(m_filterLow);
            m_filterLow = 0;
            m_filterHigh = w;
        } else if (isLsbFamily && m_filterLow >= 0 && m_filterHigh > 0) {
            // Flip: 0,2700 → -2700,0
            int w = m_filterHigh;
            m_filterLow = -w;
            m_filterHigh = 0;
        }
        filterChanged_ = true;
    }
    if (c.contains(QStringLiteral("modeList"))) {
        const QStringList modes = c.value(QStringLiteral("modeList")).toStringList();
        if (modes != m_modeList) {
            m_modeList = modes;
            emit modeListChanged(modes);
        }
    }
    if (c.contains(QStringLiteral("active"))) {
        bool a = c.value(QStringLiteral("active")).toBool();
        if (a != m_active) {
            m_active = a;
            emit activeChanged(a);
        }
    }
    if (c.contains(QStringLiteral("txSlice"))) {
        bool tx = c.value(QStringLiteral("txSlice")).toBool();
        if (tx != m_txSlice) {
            m_txSlice = tx;
            emit txSliceChanged(tx);
        }
    }
    if (c.contains(QStringLiteral("rfGain"))) {
        float g = c.value(QStringLiteral("rfGain")).toFloat();
        if (m_rfGain != g) { m_rfGain = g; emit rfGainChanged(g); }
    }
    if (c.contains(QStringLiteral("audioGain"))) {
        float g = c.value(QStringLiteral("audioGain")).toFloat();
        if (m_audioGain != g) {
            const float previousVisibleGain = audioGain();
            m_audioGain = g;
            if (audioGain() != previousVisibleGain) {
                emit audioGainChanged(audioGain());
            }
        }
    }
    if (c.contains(QStringLiteral("audioPan"))) {
        const int previousVisiblePan = audioPan();
        m_audioPan = c.value(QStringLiteral("audioPan")).toInt();
        if (audioPan() != previousVisiblePan) {
            emit audioPanChanged(audioPan());
        }
    }
    if (c.contains(QStringLiteral("audioMute"))) {
        bool mute = c.value(QStringLiteral("audioMute")).toBool();
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
    } else if (c.value(QStringLiteral("inUse")).toBool() && m_audioMute) {
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
    if (c.contains(QStringLiteral("diversityChild"))) {
        m_diversityChild = c.value(QStringLiteral("diversityChild")).toBool();
    }
    if (c.contains(QStringLiteral("diversityParent"))) {
        m_diversityParent = c.value(QStringLiteral("diversityParent")).toBool();
    }
    if (c.contains(QStringLiteral("diversity"))) {
        m_diversity = c.value(QStringLiteral("diversity")).toBool();
    }
    if (c.contains(QStringLiteral("diversityIndex"))) {
        m_diversityIndex = c.value(QStringLiteral("diversityIndex")).toInt();
    }
    if (m_diversityChild != previousDiversityChild
        || m_diversityParent != previousDiversityParent
        || m_diversity != previousDiversity
        || m_diversityIndex != previousDiversityIndex) {
        emit diversityChanged(m_diversity);
    }

    // ESC (Enhanced Signal Clarity) — diversity beamforming ("1"/"on" → bool
    // is normalized in the backend decode).
    if (c.contains(QStringLiteral("esc"))) {
        bool on = c.value(QStringLiteral("esc")).toBool();
        if (on != m_escEnabled) { m_escEnabled = on; emit escEnabledChanged(on); }
    }
    if (c.contains(QStringLiteral("escGain"))) {
        float g = c.value(QStringLiteral("escGain")).toFloat();
        if (!qFuzzyCompare(m_escGain, g)) { m_escGain = g; emit escGainChanged(g); }
    }
    if (c.contains(QStringLiteral("escPhaseShift"))) {
        float p = c.value(QStringLiteral("escPhaseShift")).toFloat();
        if (!qFuzzyCompare(m_escPhaseShift, p)) { m_escPhaseShift = p; emit escPhaseShiftChanged(p); }
    }

    // Slice control state (antenna lists are split+trimmed in the backend)
    if (c.contains(QStringLiteral("rxAntennaList"))) {
        const QStringList ants = c.value(QStringLiteral("rxAntennaList")).toStringList();
        if (ants != m_rxAntennaList) {
            m_rxAntennaList = ants;
            emit rxAntennaListChanged(m_rxAntennaList);
        }
    }
    if (c.contains(QStringLiteral("txAntennaList"))) {
        const QStringList ants = c.value(QStringLiteral("txAntennaList")).toStringList();
        if (ants != m_txAntennaList) {
            m_txAntennaList = ants;
            emit txAntennaListChanged(m_txAntennaList);
        }
    }
    if (c.contains(QStringLiteral("rxAntenna"))) {
        m_rxAntenna = c.value(QStringLiteral("rxAntenna")).toString();
        emit rxAntennaChanged(m_rxAntenna);
    }
    if (c.contains(QStringLiteral("txAntenna"))) {
        m_txAntenna = c.value(QStringLiteral("txAntenna")).toString();
        emit txAntennaChanged(m_txAntenna);
    }
    if (c.contains(QStringLiteral("locked"))) {
        m_locked = c.value(QStringLiteral("locked")).toBool();
        if (!m_locked) {
            m_lockedFeedbackTimer.stop();
            setLockedFeedbackActive(false);
        }
        emit lockedChanged(m_locked);
    }
    if (c.contains(QStringLiteral("qsk"))) {
        m_qsk = c.value(QStringLiteral("qsk")).toBool();
        emit qskChanged(m_qsk);
    }
    if (c.contains(QStringLiteral("nb"))) {
        m_nb = c.value(QStringLiteral("nb")).toBool();
        emit nbChanged(m_nb);
    }
    if (c.contains(QStringLiteral("nr"))) {
        m_nr = c.value(QStringLiteral("nr")).toBool();
        emit nrChanged(m_nr);
    }
    if (c.contains(QStringLiteral("anf"))) {
        m_anf = c.value(QStringLiteral("anf")).toBool();
        emit anfChanged(m_anf);
    }
    if (c.contains(QStringLiteral("nrl"))) {
        m_nrl = c.value(QStringLiteral("nrl")).toBool();
        emit nrlChanged(m_nrl);
    }
    if (c.contains(QStringLiteral("nrs"))) {
        m_nrs = c.value(QStringLiteral("nrs")).toBool();
        emit nrsChanged(m_nrs);
    }
    if (c.contains(QStringLiteral("rnn"))) {
        m_rnn = c.value(QStringLiteral("rnn")).toBool();
        emit rnnChanged(m_rnn);
    }
    if (c.contains(QStringLiteral("nrf"))) {
        m_nrf = c.value(QStringLiteral("nrf")).toBool();
        emit nrfChanged(m_nrf);
    }
    if (c.contains(QStringLiteral("anfl"))) {
        m_anfl = c.value(QStringLiteral("anfl")).toBool();
        emit anflChanged(m_anfl);
    }
    if (c.contains(QStringLiteral("anft"))) {
        m_anft = c.value(QStringLiteral("anft")).toBool();
        emit anftChanged(m_anft);
    }
    if (c.contains(QStringLiteral("apf"))) {
        bool v = c.value(QStringLiteral("apf")).toBool();
        if (m_apf != v) { m_apf = v; emit apfChanged(v); }
    }
    if (c.contains(QStringLiteral("apfLevel"))) {
        int v = c.value(QStringLiteral("apfLevel")).toInt();
        if (m_apfLevel != v) { m_apfLevel = v; emit apfLevelChanged(v); }
    }
    // DSP levels
    if (c.contains(QStringLiteral("nbLevel"))) {
        int v = c.value(QStringLiteral("nbLevel")).toInt();
        if (m_nbLevel != v) { m_nbLevel = v; emit nbLevelChanged(v); }
    }
    if (c.contains(QStringLiteral("nrLevel"))) {
        int v = c.value(QStringLiteral("nrLevel")).toInt();
        if (m_nrLevel != v) { m_nrLevel = v; emit nrLevelChanged(v); }
    }
    if (c.contains(QStringLiteral("anfLevel"))) {
        int v = c.value(QStringLiteral("anfLevel")).toInt();
        if (m_anfLevel != v) { m_anfLevel = v; emit anfLevelChanged(v); }
    }
    if (c.contains(QStringLiteral("nrlLevel"))) {
        int v = c.value(QStringLiteral("nrlLevel")).toInt();
        if (m_nrlLevel != v) { m_nrlLevel = v; emit nrlLevelChanged(v); }
    }
    if (c.contains(QStringLiteral("nrsLevel"))) {
        int v = c.value(QStringLiteral("nrsLevel")).toInt();
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
    if (c.contains(QStringLiteral("nrfLevel"))) {
        int v = c.value(QStringLiteral("nrfLevel")).toInt();
        if (m_nrfLevel != v) { m_nrfLevel = v; emit nrfLevelChanged(v); }
    }
    if (c.contains(QStringLiteral("anflLevel"))) {
        int v = c.value(QStringLiteral("anflLevel")).toInt();
        if (m_anflLevel != v) { m_anflLevel = v; emit anflLevelChanged(v); }
    }
    if (c.contains(QStringLiteral("agcMode"))) {
        m_agcMode = c.value(QStringLiteral("agcMode")).toString();
        emit agcModeChanged(m_agcMode);
    }
    if (c.contains(QStringLiteral("agcThreshold"))) {
        m_agcThreshold = c.value(QStringLiteral("agcThreshold")).toInt();
        emit agcThresholdChanged(m_agcThreshold);
    }
    if (c.contains(QStringLiteral("agcOffLevel"))) {
        m_agcOffLevel = c.value(QStringLiteral("agcOffLevel")).toInt();
        emit agcOffLevelChanged(m_agcOffLevel);
    }
    if (c.contains(QStringLiteral("squelchOn")) || c.contains(QStringLiteral("squelchLevel"))) {
        if (c.contains(QStringLiteral("squelchOn")))
            m_squelchOn = c.value(QStringLiteral("squelchOn")).toBool();
        if (c.contains(QStringLiteral("squelchLevel")))
            m_squelchLevel = c.value(QStringLiteral("squelchLevel")).toInt();
        emit squelchChanged(m_squelchOn, m_squelchLevel);
    }
    if (c.contains(QStringLiteral("ritOn")) || c.contains(QStringLiteral("ritFreq"))) {
        if (c.contains(QStringLiteral("ritOn")))   m_ritOn   = c.value(QStringLiteral("ritOn")).toBool();
        if (c.contains(QStringLiteral("ritFreq"))) m_ritFreq = c.value(QStringLiteral("ritFreq")).toInt();
        emit ritChanged(m_ritOn, m_ritFreq);
    }
    if (c.contains(QStringLiteral("xitOn")) || c.contains(QStringLiteral("xitFreq"))) {
        if (c.contains(QStringLiteral("xitOn")))   m_xitOn   = c.value(QStringLiteral("xitOn")).toBool();
        if (c.contains(QStringLiteral("xitFreq"))) m_xitFreq = c.value(QStringLiteral("xitFreq")).toInt();
        emit xitChanged(m_xitOn, m_xitFreq);
    }
    if (c.contains(QStringLiteral("daxChannel"))) {
        int ch = c.value(QStringLiteral("daxChannel")).toInt();
        if (m_daxChannel != ch) { m_daxChannel = ch; emit daxChannelChanged(ch); }
    }
    if (c.contains(QStringLiteral("rttyMark"))) {
        int v = c.value(QStringLiteral("rttyMark")).toInt();
        // The radio resets rtty_mark to 2125 on band changes regardless of the
        // configured rtty_mark_default. If we know the default differs and the
        // user has not explicitly chosen 2125, push the default back.
        if (v == 2125 && m_rttyMarkDefault != 2125 && !m_rttyMarkUserOverride) {
            v = m_rttyMarkDefault;
            sendCommand(QString("slice set %1 rtty_mark=%2").arg(m_id).arg(v));
        }
        if (m_rttyMark != v) { m_rttyMark = v; emit rttyMarkChanged(v); }
    }
    if (c.contains(QStringLiteral("rttyShift"))) {
        int v = c.value(QStringLiteral("rttyShift")).toInt();
        if (m_rttyShift != v) { m_rttyShift = v; emit rttyShiftChanged(v); }
    }
    if (c.contains(QStringLiteral("diglOffset"))) {
        int v = c.value(QStringLiteral("diglOffset")).toInt();
        if (m_diglOffset != v) { m_diglOffset = v; emit diglOffsetChanged(v); }
    }
    if (c.contains(QStringLiteral("diguOffset"))) {
        int v = c.value(QStringLiteral("diguOffset")).toInt();
        if (m_diguOffset != v) { m_diguOffset = v; emit diguOffsetChanged(v); }
    }

    // Record/playback status
    if (c.contains(QStringLiteral("recordOn"))) {
        bool on = c.value(QStringLiteral("recordOn")).toBool();
        if (m_recordOn != on) { m_recordOn = on; emit recordOnChanged(on); }
    }
    if (c.contains(QStringLiteral("play"))) {
        const QString v = c.value(QStringLiteral("play")).toString();
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
    if (c.contains(QStringLiteral("fmToneMode"))) {
        m_fmToneMode = c.value(QStringLiteral("fmToneMode")).toString();
        emit fmToneModeChanged(m_fmToneMode);
    }
    if (c.contains(QStringLiteral("fmToneValue"))) {
        double v = c.value(QStringLiteral("fmToneValue")).toDouble();
        m_fmToneValue = QString::number(v, 'f', 1);
        emit fmToneValueChanged(m_fmToneValue);
    }
    if (c.contains(QStringLiteral("repeaterOffsetDir"))) {
        m_repeaterOffsetDir = c.value(QStringLiteral("repeaterOffsetDir")).toString();
        emit repeaterOffsetDirChanged(m_repeaterOffsetDir);
    }
    if (c.contains(QStringLiteral("fmRepeaterOffsetFreq"))) {
        m_fmRepeaterOffsetFreq = c.value(QStringLiteral("fmRepeaterOffsetFreq")).toDouble();
        emit fmRepeaterOffsetFreqChanged(m_fmRepeaterOffsetFreq);
    }
    if (c.contains(QStringLiteral("txOffsetFreq"))) {
        m_txOffsetFreq = c.value(QStringLiteral("txOffsetFreq")).toDouble();
        emit txOffsetFreqChanged(m_txOffsetFreq);
    }
    if (c.contains(QStringLiteral("fmDeviation"))) {
        m_fmDeviation = c.value(QStringLiteral("fmDeviation")).toInt();
        emit fmDeviationChanged(m_fmDeviation);
    }

    if (c.contains(QStringLiteral("step")) || c.contains(QStringLiteral("stepList"))) {
        bool changed = false;
        if (c.contains(QStringLiteral("step"))) {
            int s = c.value(QStringLiteral("step")).toInt();
            if (s != m_stepHz) { m_stepHz = s; changed = true; }
        }
        if (c.contains(QStringLiteral("stepList"))) {
            QVector<int> list;
            for (const auto& v : c.value(QStringLiteral("stepList")).toString().split(','))
                if (!v.isEmpty()) list.append(v.toInt());
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
