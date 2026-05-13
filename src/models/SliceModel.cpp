#include "SliceModel.h"
#include <QDebug>

namespace AetherSDR {

namespace {

QStringList splitAntennaList(const QString& value)
{
    QStringList result;
    for (QString token : value.split(',', Qt::SkipEmptyParts)) {
        token = token.trimmed();
        if (!token.isEmpty())
            result.append(token);
    }
    return result;
}

} // namespace

SliceModel::SliceModel(int id, QObject* parent)
    : QObject(parent), m_id(id)
{}

// ─── Setters ──────────────────────────────────────────────────────────────────

// Helper: emit commandReady to send the command immediately (when connected),
// or queue it for when the connection becomes available.
void SliceModel::sendCommand(const QString& cmd)
{
    emit commandReady(cmd);
}

void SliceModel::setFrequency(double mhz)
{
    if (m_locked) return;
    if (qFuzzyCompare(m_frequency, mhz)) return;
    m_frequency = mhz;
    // autopan=0 prevents the radio from recentering the pan (#292).
    // SmartSDR pcap confirms: scroll-wheel uses "slice tune <id> <freq> autopan=0".
    sendCommand(QString("slice tune %1 %2 autopan=0").arg(m_id).arg(mhz, 0, 'f', 6));
    emit frequencyChanged(mhz);
}

void SliceModel::tuneAndRecenter(double mhz)
{
    if (m_locked) return;
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
    sendCommand(QString("slice set %1 mode=%2").arg(m_id).arg(mode));
    emit modeChanged(mode);
}

void SliceModel::setFilterWidth(int low, int high)
{
    m_filterLow  = low;
    m_filterHigh = high;
    // FlexAPI: "filt <id> <low_hz> <high_hz>"
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
    emit lockedChanged(locked);
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
    if (m_agcMode == mode) return;
    m_agcMode = mode;
    sendCommand(QString("slice set %1 agc_mode=%2").arg(m_id).arg(mode));
    emit agcModeChanged(mode);
}

void SliceModel::setAgcThreshold(int value)
{
    value = qBound(0, value, 100);
    if (m_agcThreshold == value) return;
    m_agcThreshold = value;
    sendCommand(QString("slice set %1 agc_threshold=%2").arg(m_id).arg(value));
    emit agcThresholdChanged(value);
}

void SliceModel::setAgcOffLevel(int value)
{
    value = qBound(0, value, 100);
    if (m_agcOffLevel == value) return;
    m_agcOffLevel = value;
    sendCommand(QString("slice set %1 agc_off_level=%2").arg(m_id).arg(value));
    emit agcOffLevelChanged(value);
}

void SliceModel::setSquelch(bool on, int level)
{
    m_squelchOn    = on;
    m_squelchLevel = level;
    sendCommand(QString("slice set %1 squelch=%2 squelch_level=%3")
                    .arg(m_id).arg(on ? 1 : 0).arg(level));
    emit squelchChanged(on, level);
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
    if (m_audioMute == mute) return;
    m_audioMute = mute;
    sendCommand(QString("slice set %1 audio_mute=%2").arg(m_id).arg(mute ? 1 : 0));
    emit audioMuteChanged(mute);
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
    if (m_audioPan == pan) return;
    m_audioPan = pan;
    sendCommand(QString("slice set %1 audio_pan=%2").arg(m_id).arg(pan));
    emit audioPanChanged(pan);
}

// ─── Status updates from radio ────────────────────────────────────────────────

void SliceModel::applyStatus(const QMap<QString, QString>& kvs)
{
    bool freqChanged   = false;
    bool modeChanged_  = false;
    bool filterChanged_= false;

    // Panadapter assignment (e.g. "pan=0x40000000")
    if (kvs.contains("pan")) {
        const QString p = kvs["pan"];
        if (m_panId != p) {
            m_panId = p;
            emit panIdChanged(m_panId);
        }
    }

    // The radio sends the frequency as "RF_frequency" in status messages.
    if (kvs.contains("RF_frequency")) {
        const double f = kvs["RF_frequency"].toDouble();
        // qFuzzyCompare fails when either value is 0.0 — use explicit epsilon
        if (std::abs(m_frequency - f) > 1e-9) {
            m_frequency = f;
            freqChanged = true;
            // Band change clears any user override so rtty_mark_default is
            // restored if the radio resets the mark in the same status update.
            m_rttyMarkUserOverride = false;
        }
    }
    if (kvs.contains("mode")) {
        const QString m = kvs["mode"];
        if (m_mode != m) {
            m_mode = m;
            modeChanged_ = true;
        }
    }
    if (kvs.contains("filter_lo") || kvs.contains("filter_hi")) {
        m_filterLow  = kvs.value("filter_lo",  QString::number(m_filterLow)).toInt();
        m_filterHigh = kvs.value("filter_hi", QString::number(m_filterHigh)).toInt();

        // Radio sometimes sends wrong-polarity filter offsets after session
        // restore (e.g. negative offsets for USB/DIGU). Normalize based on mode.
        const bool isUsbFamily = (m_mode == "USB" || m_mode == "DIGU" || m_mode == "FDV"
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
    if (kvs.contains("mode_list")) {
        QStringList modes = kvs["mode_list"].split(',', Qt::SkipEmptyParts);
        if (modes != m_modeList) {
            m_modeList = modes;
            emit modeListChanged(modes);
        }
    }
    if (kvs.contains("active")) {
        bool a = kvs["active"] == "1";
        if (a != m_active) {
            m_active = a;
            emit activeChanged(a);
        }
    }
    if (kvs.contains("tx")) {
        bool tx = kvs["tx"] == "1";
        if (tx != m_txSlice) {
            m_txSlice = tx;
            emit txSliceChanged(tx);
        }
    }
    if (kvs.contains("rfgain")) {
        float g = kvs["rfgain"].toFloat();
        if (m_rfGain != g) { m_rfGain = g; emit rfGainChanged(g); }
    }
    if (kvs.contains("audio_level")) {
        float g = kvs["audio_level"].toFloat();
        if (m_audioGain != g) { m_audioGain = g; emit audioGainChanged(g); }
    }
    if (kvs.contains("audio_pan")) {
        m_audioPan = kvs["audio_pan"].toInt();
        emit audioPanChanged(m_audioPan);
    }
    if (kvs.contains("audio_mute")) {
        bool mute = kvs["audio_mute"] == "1";
        if (mute != m_audioMute) {
            m_audioMute = mute;
            emit audioMuteChanged(mute);
        }
    }
    // Parse child/parent flags before emitting diversityChanged so handlers
    // can check isDiversityChild() to gate ESC panel visibility.
    if (kvs.contains("diversity_child"))
        m_diversityChild = kvs["diversity_child"] == "1";
    if (kvs.contains("diversity_parent"))
        m_diversityParent = kvs["diversity_parent"] == "1";
    if (kvs.contains("diversity")) {
        bool div = kvs["diversity"] == "1";
        if (div != m_diversity) {
            m_diversity = div;
            emit diversityChanged(div);
        }
    }
    if (kvs.contains("diversity_index"))
        m_diversityIndex = kvs["diversity_index"].toInt();

    // ESC (Enhanced Signal Clarity) — diversity beamforming
    if (kvs.contains("esc")) {
        const QString& v = kvs["esc"];
        bool on = (v == "1" || v == "on");
        if (on != m_escEnabled) { m_escEnabled = on; emit escEnabledChanged(on); }
    }
    if (kvs.contains("esc_gain")) {
        float g = kvs["esc_gain"].toFloat();
        if (!qFuzzyCompare(m_escGain, g)) { m_escGain = g; emit escGainChanged(g); }
    }
    if (kvs.contains("esc_phase_shift")) {
        float p = kvs["esc_phase_shift"].toFloat();
        if (!qFuzzyCompare(m_escPhaseShift, p)) { m_escPhaseShift = p; emit escPhaseShiftChanged(p); }
    }

    // Slice control state
    if (kvs.contains("ant_list") || kvs.contains("rx_ant_list")) {
        const QString raw = kvs.value("rx_ant_list", kvs.value("ant_list"));
        const QStringList ants = splitAntennaList(raw);
        if (ants != m_rxAntennaList) {
            m_rxAntennaList = ants;
            emit rxAntennaListChanged(m_rxAntennaList);
        }
    }
    if (kvs.contains("tx_ant_list")) {
        const QStringList ants = splitAntennaList(kvs["tx_ant_list"]);
        if (ants != m_txAntennaList) {
            m_txAntennaList = ants;
            emit txAntennaListChanged(m_txAntennaList);
        }
    }
    if (kvs.contains("rxant")) {
        m_rxAntenna = kvs["rxant"];
        emit rxAntennaChanged(m_rxAntenna);
    }
    if (kvs.contains("txant")) {
        m_txAntenna = kvs["txant"];
        emit txAntennaChanged(m_txAntenna);
    }
    // Status key is "lock" (not "locked") per FlexAPI
    if (kvs.contains("lock")) {
        m_locked = kvs["lock"] == "1";
        emit lockedChanged(m_locked);
    }
    if (kvs.contains("qsk")) {
        m_qsk = kvs["qsk"] == "1";
        emit qskChanged(m_qsk);
    }
    if (kvs.contains("nb")) {
        m_nb = kvs["nb"] == "1";
        emit nbChanged(m_nb);
    }
    if (kvs.contains("nr")) {
        m_nr = kvs["nr"] == "1";
        emit nrChanged(m_nr);
    }
    if (kvs.contains("anf")) {
        m_anf = kvs["anf"] == "1";
        emit anfChanged(m_anf);
    }
    if (kvs.contains("nrl")) {
        m_nrl = kvs["nrl"] == "1";
        emit nrlChanged(m_nrl);
    }
    if (kvs.contains("nrs")) {
        m_nrs = kvs["nrs"] == "1";
        emit nrsChanged(m_nrs);
    }
    if (kvs.contains("rnn")) {
        m_rnn = kvs["rnn"] == "1";
        emit rnnChanged(m_rnn);
    }
    if (kvs.contains("nrf")) {
        m_nrf = kvs["nrf"] == "1";
        emit nrfChanged(m_nrf);
    }
    if (kvs.contains("anfl")) {
        m_anfl = kvs["anfl"] == "1";
        emit anflChanged(m_anfl);
    }
    if (kvs.contains("anft")) {
        m_anft = kvs["anft"] == "1";
        emit anftChanged(m_anft);
    }
    if (kvs.contains("apf")) {
        bool v = kvs["apf"] == "1";
        if (m_apf != v) { m_apf = v; emit apfChanged(v); }
    }
    if (kvs.contains("apf_level")) {
        int v = kvs["apf_level"].toInt();
        if (m_apfLevel != v) { m_apfLevel = v; emit apfLevelChanged(v); }
    }
    // DSP level parsing
    if (kvs.contains("nb_level")) {
        int v = kvs["nb_level"].toInt();
        if (m_nbLevel != v) { m_nbLevel = v; emit nbLevelChanged(v); }
    }
    if (kvs.contains("nr_level")) {
        int v = kvs["nr_level"].toInt();
        if (m_nrLevel != v) { m_nrLevel = v; emit nrLevelChanged(v); }
    }
    if (kvs.contains("anf_level")) {
        int v = kvs["anf_level"].toInt();
        if (m_anfLevel != v) { m_anfLevel = v; emit anfLevelChanged(v); }
    }
    if (kvs.contains("lms_nr_level")) {
        int v = kvs["lms_nr_level"].toInt();
        if (m_nrlLevel != v) { m_nrlLevel = v; emit nrlLevelChanged(v); }
    }
    if (kvs.contains("speex_nr_level")) {
        int v = kvs["speex_nr_level"].toInt();
        if (m_nrsLevel != v) { m_nrsLevel = v; emit nrsLevelChanged(v); }
    }
    if (kvs.contains("nrf_level")) {
        int v = kvs["nrf_level"].toInt();
        if (m_nrfLevel != v) { m_nrfLevel = v; emit nrfLevelChanged(v); }
    }
    if (kvs.contains("lms_anf_level")) {
        int v = kvs["lms_anf_level"].toInt();
        if (m_anflLevel != v) { m_anflLevel = v; emit anflLevelChanged(v); }
    }
    if (kvs.contains("agc_mode")) {
        m_agcMode = kvs["agc_mode"];
        emit agcModeChanged(m_agcMode);
    }
    if (kvs.contains("agc_threshold")) {
        m_agcThreshold = kvs["agc_threshold"].toInt();
        emit agcThresholdChanged(m_agcThreshold);
    }
    if (kvs.contains("agc_off_level")) {
        m_agcOffLevel = kvs["agc_off_level"].toInt();
        emit agcOffLevelChanged(m_agcOffLevel);
    }
    if (kvs.contains("squelch") || kvs.contains("squelch_level")) {
        if (kvs.contains("squelch"))       m_squelchOn    = kvs["squelch"] == "1";
        if (kvs.contains("squelch_level")) m_squelchLevel = kvs["squelch_level"].toInt();
        emit squelchChanged(m_squelchOn, m_squelchLevel);
    }
    if (kvs.contains("rit_on") || kvs.contains("rit_freq")) {
        if (kvs.contains("rit_on"))   m_ritOn   = kvs["rit_on"] == "1";
        if (kvs.contains("rit_freq")) m_ritFreq = kvs["rit_freq"].toInt();
        emit ritChanged(m_ritOn, m_ritFreq);
    }
    if (kvs.contains("xit_on") || kvs.contains("xit_freq")) {
        if (kvs.contains("xit_on"))   m_xitOn   = kvs["xit_on"] == "1";
        if (kvs.contains("xit_freq")) m_xitFreq = kvs["xit_freq"].toInt();
        emit xitChanged(m_xitOn, m_xitFreq);
    }
    if (kvs.contains("dax")) {
        int ch = kvs["dax"].toInt();
        if (m_daxChannel != ch) { m_daxChannel = ch; emit daxChannelChanged(ch); }
    }
    if (kvs.contains("rtty_mark")) {
        int v = kvs["rtty_mark"].toInt();
        // The radio resets rtty_mark to 2125 on band changes regardless of the
        // configured rtty_mark_default. If we know the default differs and the
        // user has not explicitly chosen 2125, push the default back.
        if (v == 2125 && m_rttyMarkDefault != 2125 && !m_rttyMarkUserOverride) {
            v = m_rttyMarkDefault;
            sendCommand(QString("slice set %1 rtty_mark=%2").arg(m_id).arg(v));
        }
        if (m_rttyMark != v) { m_rttyMark = v; emit rttyMarkChanged(v); }
    }
    if (kvs.contains("rtty_shift")) {
        int v = kvs["rtty_shift"].toInt();
        if (m_rttyShift != v) { m_rttyShift = v; emit rttyShiftChanged(v); }
    }
    if (kvs.contains("digl_offset")) {
        int v = kvs["digl_offset"].toInt();
        if (m_diglOffset != v) { m_diglOffset = v; emit diglOffsetChanged(v); }
    }
    if (kvs.contains("digu_offset")) {
        int v = kvs["digu_offset"].toInt();
        if (m_diguOffset != v) { m_diguOffset = v; emit diguOffsetChanged(v); }
    }

    // Record/playback status
    if (kvs.contains("record")) {
        bool on = kvs["record"] == "1";
        if (m_recordOn != on) { m_recordOn = on; emit recordOnChanged(on); }
    }
    if (kvs.contains("play")) {
        const QString& v = kvs["play"];
        if (v == "disabled") {
            if (m_playEnabled) { m_playEnabled = false; emit playEnabledChanged(false); }
            if (m_playOn) { m_playOn = false; emit playOnChanged(false); }
        } else {
            if (!m_playEnabled) { m_playEnabled = true; emit playEnabledChanged(true); }
            bool on = (v == "1");
            if (m_playOn != on) { m_playOn = on; emit playOnChanged(on); }
        }
    }

    // FM duplex/repeater status
    // Normalize to lowercase / fixed decimal format to match UI combo-box item data
    if (kvs.contains("fm_tone_mode")) {
        m_fmToneMode = kvs["fm_tone_mode"].toLower();
        emit fmToneModeChanged(m_fmToneMode);
    }
    if (kvs.contains("fm_tone_value")) {
        double v = kvs["fm_tone_value"].toDouble();
        m_fmToneValue = QString::number(v, 'f', 1);
        emit fmToneValueChanged(m_fmToneValue);
    }
    if (kvs.contains("repeater_offset_dir")) {
        m_repeaterOffsetDir = kvs["repeater_offset_dir"].toLower();
        emit repeaterOffsetDirChanged(m_repeaterOffsetDir);
    }
    if (kvs.contains("fm_repeater_offset_freq")) {
        m_fmRepeaterOffsetFreq = kvs["fm_repeater_offset_freq"].toDouble();
        emit fmRepeaterOffsetFreqChanged(m_fmRepeaterOffsetFreq);
    }
    if (kvs.contains("tx_offset_freq")) {
        m_txOffsetFreq = kvs["tx_offset_freq"].toDouble();
        emit txOffsetFreqChanged(m_txOffsetFreq);
    }
    if (kvs.contains("fm_deviation")) {
        m_fmDeviation = kvs["fm_deviation"].toInt();
        emit fmDeviationChanged(m_fmDeviation);
    }

    if (kvs.contains("step") || kvs.contains("step_list")) {
        bool changed = false;
        if (kvs.contains("step")) {
            int s = kvs["step"].toInt();
            if (s != m_stepHz) { m_stepHz = s; changed = true; }
        }
        if (kvs.contains("step_list")) {
            QVector<int> list;
            for (const auto& v : kvs["step_list"].split(','))
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
