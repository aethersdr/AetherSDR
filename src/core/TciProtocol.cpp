#ifdef HAVE_WEBSOCKETS
#include "TciProtocol.h"
#include "AppSettings.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"
#include "models/EqualizerModel.h"
#include "models/SpotModel.h"
#include "models/DaxIqModel.h"

#include <QList>
#include <QMetaObject>
#include <cmath>

namespace AetherSDR {

int TciProtocol::tciTrxForSlice(RadioModel* model, const SliceModel* slice)
{
    if (!model || !slice)
        return 0;
    const auto slices = model->slices();
    const int index = slices.indexOf(const_cast<SliceModel*>(slice));
    return index >= 0 ? index : slice->sliceId();
}

TciProtocol::TciProtocol(RadioModel* model)
    : m_model(model)
{}

// ── Mode conversion ────────────────────────────────────────────────────────

QString TciProtocol::smartsdrToTci(const QString& mode)
{
    static const QMap<QString, QString> map = {
        {"USB",  "usb"},   {"LSB",  "lsb"},
        {"CW",   "cw"},    {"CWL",  "cwr"},
        {"AM",   "am"},    {"SAM",  "sam"},
        {"FM",   "fm"},    {"NFM",  "nfm"},
        {"DFM",  "fm"},    {"FDM",  "fm"},
        {"DIGU", "digu"},  {"DIGL", "digl"},
        {"RTTY", "rtty"},  {"FDV",  "digu"},
    };
    return map.value(mode.toUpper(), "usb");
}

QString TciProtocol::tciToSmartSDR(const QString& mode)
{
    static const QMap<QString, QString> map = {
        {"usb",  "USB"},   {"lsb",  "LSB"},
        {"cw",   "CW"},    {"cwr",  "CWL"},
        {"am",   "AM"},    {"sam",  "SAM"},
        {"fm",   "FM"},    {"nfm",  "NFM"},
        {"digu", "DIGU"},  {"digl", "DIGL"},
        {"rtty", "RTTY"},
    };
    return map.value(mode.toLower(), "USB");
}

// ── Helpers ────────────────────────────────────────────────────────────────

SliceModel* TciProtocol::sliceForTrx(int trx) const
{
    if (!m_model || !m_model->isConnected()) return nullptr;
    auto slices = m_model->slices();
    if (trx >= 0 && trx < slices.size())
        return slices.at(trx);

    // Compatibility fallback for clients that learned raw Flex slice ids from
    // older AetherSDR builds.
    for (auto* s : m_model->slices()) {
        if (s->sliceId() == trx) return s;
    }
    // Fallback: first slice
    return slices.isEmpty() ? nullptr : slices.first();
}

// ── Init burst ─────────────────────────────────────────────────────────────

QString TciProtocol::generateInitBurst()
{
    QString burst;
    burst += QStringLiteral("protocol:ExpertSDR3,1.5;");
    burst += QStringLiteral("device:%1;").arg(
        m_model ? m_model->name() + " " + m_model->model()
                : QStringLiteral("AetherSDR"));
    burst += QStringLiteral("receive_only:false;");

    // Count TRXs based on owned slices and expose them as contiguous TCI
    // receivers (0..N-1). Flex slice ids may start at 1 when another client
    // owns slice 0, but TCI clients use indexes within the advertised count.
    const auto slices = m_model ? m_model->slices() : QList<SliceModel*>{};
    int trxCount = slices.size();
    if (trxCount < 1) trxCount = 1;
    burst += QStringLiteral("trx_count:%1;").arg(trxCount);
    burst += QStringLiteral("channels_count:1;");

    // Modulations list
    burst += QStringLiteral("modulations_list:usb,lsb,cw,cwr,am,sam,fm,nfm,digu,digl,rtty;");

    // Per-slice state
    if (m_model) {
        for (auto* s : slices) {
            int trx = tciTrxForSlice(m_model, s);
            long long hz = static_cast<long long>(std::round(s->frequency() * 1e6));
            burst += QStringLiteral("vfo:%1,0,%2;").arg(trx).arg(hz);
            burst += QStringLiteral("modulation:%1,%2;")
                         .arg(trx).arg(smartsdrToTci(s->mode()));
            burst += QStringLiteral("rx_enable:%1,true;").arg(trx);

            // Filter
            burst += QStringLiteral("rx_filter_band:%1,%2,%3;")
                         .arg(trx).arg(s->filterLow()).arg(s->filterHigh());

            // RIT/XIT
            burst += QStringLiteral("rit_enable:%1,%2;")
                         .arg(trx).arg(s->ritOn() ? "true" : "false");
            burst += QStringLiteral("xit_enable:%1,%2;")
                         .arg(trx).arg(s->xitOn() ? "true" : "false");
            burst += QStringLiteral("rit_offset:%1,%2;")
                         .arg(trx).arg(static_cast<int>(s->ritFreq()));
            burst += QStringLiteral("xit_offset:%1,%2;")
                         .arg(trx).arg(static_cast<int>(s->xitFreq()));

            // Lock
            burst += QStringLiteral("lock:%1,%2;")
                         .arg(trx).arg(s->isLocked() ? "true" : "false");

            // SQL
            burst += QStringLiteral("sql_enable:%1,%2;")
                         .arg(trx).arg(s->squelchOn() ? "true" : "false");
            burst += QStringLiteral("sql_level:%1,%2;")
                         .arg(trx).arg(s->squelchLevel());

            // AGC
            burst += QStringLiteral("agc_mode:%1,%2;")
                         .arg(trx).arg(s->agcMode().toLower());

            // DSP
            burst += QStringLiteral("rx_nb_enable:%1,%2;")
                         .arg(trx).arg(s->nbOn() ? "true" : "false");
            burst += QStringLiteral("rx_nr_enable:%1,%2;")
                         .arg(trx).arg(s->nrOn() ? "true" : "false");
            burst += QStringLiteral("rx_anf_enable:%1,%2;")
                         .arg(trx).arg(s->anfOn() ? "true" : "false");
            burst += QStringLiteral("rx_apf_enable:%1,%2;")
                         .arg(trx).arg(s->apfOn() ? "true" : "false");

            // TX
            burst += QStringLiteral("tx_enable:%1,%2;")
                         .arg(trx).arg(s->isTxSlice() ? "true" : "false");
        }

        // Global TX state
        auto& tx = m_model->transmitModel();
        bool isTx = tx.isTransmitting();
        int txTrx = 0;
        for (auto* s : slices) {
            if (s->isTxSlice()) {
                txTrx = tciTrxForSlice(m_model, s);
                break;
            }
        }
        // ESDR3 format requires TRX index prefix for drive commands.
        // Without it, WSJT-X/JTDX crash parsing args.at(1) on a 1-element list.
        burst += QStringLiteral("drive:%1,%2;").arg(txTrx).arg(tx.rfPower());
        burst += QStringLiteral("tune_drive:%1,%2;").arg(txTrx).arg(tx.tunePower());
        burst += QStringLiteral("trx:%1,%2;").arg(txTrx).arg(isTx ? "true" : "false");
    }

    burst += QStringLiteral("audio_samplerate:48000;");
    // Stream negotiation — required for WSJT-X to initialize its TCI TX
    // audio FIFO. Without these, readAudioData() returns all zeros.
    // Matches Thetis TCI server init burst. — confirmed via Thetis source
    burst += QStringLiteral("audio_stream_sample_type:float32;");
    burst += QStringLiteral("audio_stream_channels:2;");
    burst += QStringLiteral("audio_stream_samples:2048;");
    burst += QStringLiteral("tx_stream_audio_buffering:50;");
    burst += QStringLiteral("iq_samplerate:48000;");
    // audio_start primes WSJT-X's TCI audio state machine so it sends
    // audio_start:0 back to request RX audio streaming.
    burst += QStringLiteral("audio_start;");
    burst += QStringLiteral("start;");
    burst += QStringLiteral("ready;");

    return burst;
}

// ── Command dispatch ───────────────────────────────────────────────────────

QString TciProtocol::handleCommand(const QString& cmd)
{
    m_pendingNotification.clear();
    m_pendingMasterVolume = -1;

    if (cmd.isEmpty()) return {};

    // Parse: COMMAND_NAME:arg1,arg2,...
    // or just: COMMAND_NAME
    int colonIdx = cmd.indexOf(':');
    QString name = (colonIdx >= 0) ? cmd.left(colonIdx).toLower().trimmed()
                                   : cmd.toLower().trimmed();
    QStringList args;
    if (colonIdx >= 0) {
        QString argStr = cmd.mid(colonIdx + 1).trimmed();
        args = argStr.split(',', Qt::KeepEmptyParts);
        for (auto& a : args) a = a.trimmed();
    }

    // Determine if this is a set or get. TCI convention is: 0-1 args = GET
    // (no args, or trx index only), 2+ args = SET (trx index + value(s)).
    // Computed once up front so all dispatched commands see the correct
    // value — the previous implementation only set this AFTER the first
    // dispatch block, leaving early-dispatched commands (rx_volume,
    // cw_keyer_speed, mon_volume, rx_mute, rx_balance, …) with isSet=false
    // forever and silently treating SETs as GETs. See issue #1764.
    bool isSet = (args.size() >= 2);

    // Commands with special handling
    if (name == "start")            return cmdStart();
    if (name == "stop")             return cmdStop();
    if (name == "tx_enable")        return cmdTxEnable(args);
    if (name == "cw_msg")           return cmdCwMsg(args);
    if (name == "cw_macros")        return cmdCwMacros(args);
    if (name == "cw_macros_stop")   return cmdCwMacrosStop();
    if (name == "spot")             return cmdSpot(args);
    if (name == "spot_delete")      return cmdSpotDelete(args);
    if (name == "spot_clear")       return cmdSpotClear();
    if (name == "iq_start")         return cmdIqStart(args);
    if (name == "iq_stop")          return cmdIqStop(args);
    if (name == "iq_samplerate")    return cmdIqSampleRate(args, args.size() >= 1 && !args[0].isEmpty());
    if (name == "keyer")            return cmdKeyer(args);
    if (name == "cw_keyer_speed")   return cmdCwKeyerSpeed(args, isSet);
    if (name == "cw_macros_delay")  return cmdCwMacrosDelay(args, isSet);
    if (name == "cw_terminal")      return cmdCwTerminal(args, isSet);
    if (name == "dds")              return cmdDds(args, isSet);
    if (name == "if")               return cmdIf(args, isSet);
    if (name == "rx_channel_enable") return cmdRxChannelEnable(args, isSet);
    if (name == "rx_volume")        return cmdRxVolume(args, isSet);
    if (name == "rx_mute")          return cmdRxMute(args, isSet);
    if (name == "rx_balance")       return cmdRxBalance(args, isSet);
    if (name == "mon_enable")       return cmdMonEnable(args, isSet);
    if (name == "mon_volume")       return cmdMonVolume(args, isSet);
    if (name == "rx_nb_param")      return cmdRxNbParam(args, isSet);
    if (name == "rx_bin_enable")    return cmdRxBinEnable(args, isSet);
    if (name == "rx_anc_enable")    return cmdRxAncEnable(args, isSet);
    if (name == "rx_dse_enable")    return cmdRxDseEnable(args, isSet);
    if (name == "rx_nf_enable")     return cmdRxNfEnable(args, isSet);
    if (name == "digl_offset")      return cmdDiglOffset(args, isSet);
    if (name == "digu_offset")      return cmdDiguOffset(args, isSet);
    if (name == "set_in_focus")     return cmdSetInFocus();
    if (name == "tx_frequency")     return cmdTxFrequency();
    if (name == "vfo_limits")       return QStringLiteral("vfo_limits:30000,54000000;");
    if (name == "if_limits")        return QStringLiteral("if_limits:-10000,10000;");
    if (name == "vfo_lock")         return cmdLock(args, isSet);  // alias

    // Bidirectional commands — isSet was already computed at the top of
    // this function, so the dispatch is uniform from here on.

    if (name == "vfo")              return cmdVfo(args, isSet);
    if (name == "modulation")       return cmdModulation(args, isSet);
    if (name == "trx")              return cmdTrx(args, isSet);
    if (name == "tune")             return cmdTune(args, isSet);
    if (name == "drive")            return cmdDrive(args, isSet);
    if (name == "tune_drive")       return cmdTuneDrive(args, isSet);
    if (name == "rit_enable")       return cmdRitEnable(args, isSet);
    if (name == "xit_enable")       return cmdXitEnable(args, isSet);
    if (name == "rit_offset")       return cmdRitOffset(args, isSet);
    if (name == "xit_offset")       return cmdXitOffset(args, isSet);
    if (name == "split_enable")     return cmdSplitEnable(args, isSet);
    if (name == "rx_filter_band")   return cmdRxFilterBand(args, isSet);
    if (name == "cw_macros_speed")  return cmdCwMacrosSpeed(args, isSet);
    if (name == "lock")             return cmdLock(args, isSet);
    if (name == "sql_enable")       return cmdSqlEnable(args, isSet);
    if (name == "sql_level")        return cmdSqlLevel(args, isSet);
    if (name == "volume")           return cmdVolume(args, isSet);
    if (name == "mute")             return cmdMute(args, isSet);
    if (name == "agc_mode")         return cmdAgcMode(args, isSet);
    if (name == "agc_gain")         return cmdAgcGain(args, isSet);
    if (name == "rx_nb_enable")     return cmdRxNbEnable(args, isSet);
    if (name == "rx_nr_enable")     return cmdRxNrEnable(args, isSet);
    if (name == "rx_anf_enable")    return cmdRxAnfEnable(args, isSet);
    if (name == "rx_apf_enable")    return cmdRxApfEnable(args, isSet);

    // AetherSDR extensions (not in TCI v2.0 spec)
    if (name == "rx_record")        return cmdRxRecord(args, isSet);
    if (name == "rx_play")          return cmdRxPlay(args, isSet);

    // Unknown command — ignore silently per TCI spec
    return {};
}

QString TciProtocol::pendingNotification()
{
    QString n = m_pendingNotification;
    m_pendingNotification.clear();
    return n;
}

// ── Command implementations ────────────────────────────────────────────────

QString TciProtocol::cmdStart()
{
    m_started = true;
    return {};
}

QString TciProtocol::cmdStop()
{
    m_started = false;
    return {};
}

// ── VFO: get/set frequency ─────────────────────────────────────────────────

QString TciProtocol::cmdVfo(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        // Get: vfo:trx,channel,freq_hz;
        long long hz = static_cast<long long>(std::round(s->frequency() * 1e6));
        return QStringLiteral("vfo:%1,0,%2;").arg(trx).arg(hz);
    }

    // Set: vfo:trx,channel,freq_hz
    if (args.size() < 3) return {};
    bool ok;
    long long hz = args[2].toLongLong(&ok);
    if (!ok || hz < 0) return {};
    double mhz = hz / 1e6;

    QMetaObject::invokeMethod(s, [s, mhz]() {
        s->tuneAndRecenter(mhz);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("vfo:%1,0,%2;").arg(trx).arg(hz);
    return {};
}

// ── Modulation: get/set mode ───────────────────────────────────────────────

QString TciProtocol::cmdModulation(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("modulation:%1,%2;")
                   .arg(trx).arg(smartsdrToTci(s->mode()));
    }

    if (args.size() < 2) return {};
    QString sdrMode = tciToSmartSDR(args[1]);
    QMetaObject::invokeMethod(s, [s, sdrMode]() {
        s->setMode(sdrMode);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("modulation:%1,%2;")
                                .arg(trx).arg(args[1].toLower());
    return {};
}

// ── TRX: get/set TX state ──────────────────────────────────────────────────

QString TciProtocol::cmdTrx(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();

    if (!isSet) {
        bool tx = m_model && m_model->transmitModel().isTransmitting();
        return QStringLiteral("trx:%1,%2;").arg(trx).arg(tx ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool tx = (args[1].toLower() == "true");
    QMetaObject::invokeMethod(m_model, [this, tx, trx]() {
        // Assign TX to this TRX's slice before keying
        if (tx) {
            auto* s = sliceForTrx(trx);
            if (s && !s->isTxSlice())
                s->setTxSlice(true);
        }
        m_model->setTransmit(tx);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("trx:%1,%2;")
                                .arg(trx).arg(tx ? "true" : "false");
    // Echo confirmation to sender — WSJT-X/JTDX wait for this ack
    // and report "TCI failed to set ptt" if it never arrives.
    return QStringLiteral("trx:%1,%2;").arg(trx).arg(tx ? "true" : "false");
}

// ── TX_ENABLE: assign TX to a TRX (unidirectional) ─────────────────────────

QString TciProtocol::cmdTxEnable(const QStringList& args)
{
    if (args.size() < 2) return {};
    int trx = args[0].toInt();
    bool enable = (args[1].toLower() == "true");

    if (enable) {
        auto* s = sliceForTrx(trx);
        if (s && !s->isTxSlice()) {
            QMetaObject::invokeMethod(s, [s]() {
                s->setTxSlice(true);
            }, Qt::QueuedConnection);
        }
    }

    m_pendingNotification = QStringLiteral("tx_enable:%1,%2;")
                                .arg(trx).arg(enable ? "true" : "false");
    return QStringLiteral("tx_enable:%1,%2;")
               .arg(trx).arg(enable ? "true" : "false");
}

// ── TUNE: get/set tune state ───────────────────────────────────────────────

QString TciProtocol::cmdTune(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    (void)trx;

    if (!isSet) {
        bool tuning = m_model && m_model->transmitModel().isTuning();
        return QStringLiteral("tune:%1,%2;").arg(trx).arg(tuning ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool tune = (args[1].toLower() == "true");
    QMetaObject::invokeMethod(m_model, [this, tune]() {
        if (tune)
            m_model->transmitModel().startTune();
        else
            m_model->transmitModel().stopTune();
    }, Qt::QueuedConnection);

    return {};
}

// ── DRIVE: get/set RF power ────────────────────────────────────────────────

// DRIVE and TUNE_DRIVE are global commands per the TCI v2.0 spec — they have
// no TRX prefix. Spec form: `drive;` for GET, `drive:N;` for SET. The
// dispatcher's isSet heuristic (args.size() >= 2) is wrong for globals
// (treats `drive:50;` as a GET). We override here: any args at all = SET,
// no args = GET. We also accept the legacy TRX-prefixed form `drive:0,N;`
// for backward compatibility — when 2+ args arrive, we take args[1] as the
// value and ignore the trx index (drive is global anyway).
QString TciProtocol::cmdDrive(const QStringList& args, bool /*isSet*/)
{
    if (args.isEmpty()) {
        int pwr = m_model ? m_model->transmitModel().rfPower() : 0;
        return QStringLiteral("drive:%1;").arg(pwr);
    }
    bool ok;
    int pwr = args[args.size() == 1 ? 0 : 1].toInt(&ok);
    if (!ok) return {};
    QMetaObject::invokeMethod(m_model, [this, pwr]() {
        m_model->transmitModel().setRfPower(pwr);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("drive:%1;").arg(pwr);
    return {};
}

// ── TUNE_DRIVE: get/set tune power ─────────────────────────────────────────

QString TciProtocol::cmdTuneDrive(const QStringList& args, bool /*isSet*/)
{
    if (args.isEmpty()) {
        int pwr = m_model ? m_model->transmitModel().tunePower() : 0;
        return QStringLiteral("tune_drive:%1;").arg(pwr);
    }
    bool ok;
    int pwr = args[args.size() == 1 ? 0 : 1].toInt(&ok);
    if (!ok) return {};
    QMetaObject::invokeMethod(m_model, [this, pwr]() {
        m_model->transmitModel().setTunePower(pwr);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("tune_drive:%1;").arg(pwr);
    return {};
}

// ── RIT ────────────────────────────────────────────────────────────────────

QString TciProtocol::cmdRitEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rit_enable:%1,%2;")
                   .arg(trx).arg(s->ritOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = (args[1].toLower() == "true");
    int hz = s->ritFreq();
    QMetaObject::invokeMethod(s, [s, on, hz]() { s->setRit(on, hz); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rit_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

QString TciProtocol::cmdRitOffset(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rit_offset:%1,%2;")
                   .arg(trx).arg(static_cast<int>(s->ritFreq()));
    }

    if (args.size() < 2) return {};
    int offset = args[1].toInt();
    bool on = s->ritOn();
    QMetaObject::invokeMethod(s, [s, on, offset]() {
        s->setRit(on, offset);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rit_offset:%1,%2;")
                                .arg(trx).arg(offset);
    return {};
}

// ── XIT ────────────────────────────────────────────────────────────────────

QString TciProtocol::cmdXitEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("xit_enable:%1,%2;")
                   .arg(trx).arg(s->xitOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = (args[1].toLower() == "true");
    int hz = s->xitFreq();
    QMetaObject::invokeMethod(s, [s, on, hz]() { s->setXit(on, hz); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("xit_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

QString TciProtocol::cmdXitOffset(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("xit_offset:%1,%2;")
                   .arg(trx).arg(static_cast<int>(s->xitFreq()));
    }

    if (args.size() < 2) return {};
    int offset = args[1].toInt();
    bool on = s->xitOn();
    QMetaObject::invokeMethod(s, [s, on, offset]() {
        s->setXit(on, offset);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("xit_offset:%1,%2;")
                                .arg(trx).arg(offset);
    return {};
}

// ── SPLIT ──────────────────────────────────────────────────────────────────

QString TciProtocol::cmdSplitEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        // Split = TX slice is different from this slice
        bool split = false;
        for (auto* sl : m_model->slices()) {
            if (sl->isTxSlice() && sl != s) { split = true; break; }
        }
        return QStringLiteral("split_enable:%1,%2;")
                   .arg(trx).arg(split ? "true" : "false");
    }

    // Set split — not fully controllable from TCI (would need to create a second slice)
    // Just acknowledge
    return {};
}

// ── RX Filter ──────────────────────────────────────────────────────────────

QString TciProtocol::cmdRxFilterBand(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_filter_band:%1,%2,%3;")
                   .arg(trx).arg(s->filterLow()).arg(s->filterHigh());
    }

    if (args.size() < 3) return {};
    int lo = args[1].toInt();
    int hi = args[2].toInt();
    QMetaObject::invokeMethod(s, [s, lo, hi]() {
        s->setFilterWidth(lo, hi);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rx_filter_band:%1,%2,%3;")
                                .arg(trx).arg(lo).arg(hi);
    return {};
}

// ── CW ─────────────────────────────────────────────────────────────────────

QString TciProtocol::cmdCwMacrosSpeed(const QStringList& args, bool isSet)
{
    if (!isSet) {
        int wpm = m_model ? m_model->transmitModel().cwSpeed() : 25;
        return QStringLiteral("cw_macros_speed:%1;").arg(wpm);
    }

    if (args.isEmpty()) return {};
    int wpm = args[0].toInt();
    if (wpm < 5 || wpm > 100) return {};
    QString cmd = QStringLiteral("cw wpm %1").arg(wpm);
    QMetaObject::invokeMethod(m_model, [this, cmd]() {
        m_model->sendCmdPublic(cmd, nullptr);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("cw_macros_speed:%1;").arg(wpm);
    return {};
}

QString TciProtocol::cmdCwMsg(const QStringList& args)
{
    if (args.isEmpty()) return {};
    // cw_msg:text — send CW macro text
    QString text = args.join(',');  // rejoin in case text had commas
    if (text.isEmpty()) return {};
    QString cmd = QStringLiteral("cwx send \"%1\"").arg(text);
    QMetaObject::invokeMethod(m_model, [this, cmd]() {
        m_model->sendCmdPublic(cmd, nullptr);
    }, Qt::QueuedConnection);
    return {};
}

// ── Lock ───────────────────────────────────────────────────────────────────

QString TciProtocol::cmdLock(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("lock:%1,%2;")
                   .arg(trx).arg(s->isLocked() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = (args[1].toLower() == "true");
    QMetaObject::invokeMethod(s, [s, on]() { s->setLocked(on); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("lock:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

// ── Squelch ────────────────────────────────────────────────────────────────

QString TciProtocol::cmdSqlEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("sql_enable:%1,%2;")
                   .arg(trx).arg(s->squelchOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = (args[1].toLower() == "true");
    int level = s->squelchLevel();
    QMetaObject::invokeMethod(s, [s, on, level]() { s->setSquelch(on, level); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("sql_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

QString TciProtocol::cmdSqlLevel(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("sql_level:%1,%2;")
                   .arg(trx).arg(s->squelchLevel());
    }

    if (args.size() < 2) return {};
    int level = args[1].toInt();
    bool on = s->squelchOn();
    QMetaObject::invokeMethod(s, [s, on, level]() { s->setSquelch(on, level); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("sql_level:%1,%2;")
                                .arg(trx).arg(level);
    return {};
}

// ── Volume / Mute ──────────────────────────────────────────────────────────

// VOLUME is a GLOBAL command per TCI v2.0 spec — it controls the master
// output level the operator hears, not a per-slice audio gain. This
// matches the title bar's master volume slider in the GUI. Per-receiver
// volume goes through the separate `rx_volume` command (cmdRxVolume).
//
// Spec form:  GET = `volume;`  (no args)
//             SET = `volume:N;` (1 arg, 0-100)
//
// We also accept the legacy TRX-prefixed form `volume:trx,N;` for
// backward compatibility — the trx is ignored since master volume is
// global. Old clients that previously sent this form to set per-slice
// gain were silently broken (the SET path mixed up vol and trx
// indices) — those clients should migrate to `rx_volume:trx,N;`.
//
// For SET, we don't directly call AudioEngine here — TciProtocol owns
// only the RadioModel, not AudioEngine. Instead we stash the requested
// level in m_pendingMasterVolume; TciServer reads it after handleCommand
// and forwards the request to MainWindow via signal, mirroring the path
// taken by the title bar's masterVolumeChanged signal.
QString TciProtocol::cmdVolume(const QStringList& args, bool /*isSet*/)
{
    if (args.isEmpty()) {
        // GET — current master volume from saved settings (the same value
        // the title bar slider reads on startup).
        int pct = AppSettings::instance()
                      .value("MasterVolume", "100").toInt();
        return QStringLiteral("volume:%1;").arg(pct);
    }

    // SET — accept either spec form (1 arg) or legacy trx-prefixed (2+ args).
    int vol = args[args.size() == 1 ? 0 : 1].toInt();
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    m_pendingMasterVolume = vol;

    m_pendingNotification = QStringLiteral("volume:%1;").arg(vol);
    return {};
}

QString TciProtocol::cmdMute(const QStringList& args, bool isSet)
{
    if (!isSet) {
        if (!args.isEmpty()) {
            int trx = args[0].toInt();
            auto* s = sliceForTrx(trx);
            if (s) return QStringLiteral("mute:%1,%2;")
                              .arg(trx).arg(s->audioMute() ? "true" : "false");
        }
        return {};
    }

    if (args.size() < 2) return {};
    int trx = args[0].toInt();
    bool mute = (args[1].toLower() == "true");
    auto* s = sliceForTrx(trx);
    if (s) {
        QMetaObject::invokeMethod(s, [s, mute]() { s->setAudioMute(mute); },
                                  Qt::QueuedConnection);
    }

    m_pendingNotification = QStringLiteral("mute:%1,%2;")
                                .arg(trx).arg(mute ? "true" : "false");
    return {};
}

// ── AGC ────────────────────────────────────────────────────────────────────

QString TciProtocol::cmdAgcMode(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("agc_mode:%1,%2;")
                   .arg(trx).arg(s->agcMode().toLower());
    }

    if (args.size() < 2) return {};
    QString mode = args[1].toLower();
    // Map TCI AGC names to FlexRadio: off, slow, med, fast
    QMetaObject::invokeMethod(s, [s, mode]() { s->setAgcMode(mode); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("agc_mode:%1,%2;")
                                .arg(trx).arg(mode);
    return {};
}

QString TciProtocol::cmdAgcGain(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("agc_gain:%1,%2;")
                   .arg(trx).arg(s->agcThreshold());
    }

    if (args.size() < 2) return {};
    int gain = args[1].toInt();
    QMetaObject::invokeMethod(s, [s, gain]() { s->setAgcThreshold(gain); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("agc_gain:%1,%2;")
                                .arg(trx).arg(gain);
    return {};
}

// ── DSP toggles ────────────────────────────────────────────────────────────

QString TciProtocol::cmdRxNbEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_nb_enable:%1,%2;")
                   .arg(trx).arg(s->nbOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = (args[1].toLower() == "true");
    QMetaObject::invokeMethod(s, [s, on]() { s->setNb(on); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rx_nb_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

QString TciProtocol::cmdRxNrEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_nr_enable:%1,%2;")
                   .arg(trx).arg(s->nrOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = (args[1].toLower() == "true");
    QMetaObject::invokeMethod(s, [s, on]() { s->setNr(on); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rx_nr_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

QString TciProtocol::cmdRxAnfEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_anf_enable:%1,%2;")
                   .arg(trx).arg(s->anfOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = (args[1].toLower() == "true");
    QMetaObject::invokeMethod(s, [s, on]() { s->setAnf(on); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rx_anf_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

QString TciProtocol::cmdRxApfEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_apf_enable:%1,%2;")
                   .arg(trx).arg(s->apfOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = (args[1].toLower() == "true");
    QMetaObject::invokeMethod(s, [s, on]() { s->setApf(on); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rx_apf_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

// ── AetherSDR extensions (DVK record/play) ─────────────────────────────────

QString TciProtocol::cmdRxRecord(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_record:%1,%2;")
                   .arg(trx).arg(s->recordOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = (args[1].toLower() == "true");
    QMetaObject::invokeMethod(s, [s, on]() { s->setRecordOn(on); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rx_record:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

QString TciProtocol::cmdRxPlay(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_play:%1,%2;")
                   .arg(trx).arg(s->playOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = (args[1].toLower() == "true");
    QMetaObject::invokeMethod(s, [s, on]() { s->setPlayOn(on); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rx_play:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

// ── Spot injection ─────────────────────────────────────────────────────────

QString TciProtocol::cmdSpot(const QStringList& args)
{
    // spot:callsign,modulation,freq_hz,color,text
    if (!m_model || args.size() < 3) return {};

    QString callsign = args[0].trimmed();
    QString mode = args[1].trimmed();
    bool ok;
    double freqHz = args[2].toDouble(&ok);
    if (!ok || callsign.isEmpty()) return {};

    QString color = (args.size() > 3) ? args[3].trimmed() : QString();
    QString comment = (args.size() > 4) ? args[4].trimmed() : QString();

    QMap<QString, QString> kvs;
    kvs["callsign"] = callsign;
    kvs["rx_freq"] = QString::number(freqHz / 1e6, 'f', 6);
    kvs["mode"] = mode;
    kvs["source"] = "TCI";
    if (!color.isEmpty()) kvs["color"] = color;
    if (!comment.isEmpty()) kvs["comment"] = comment;
    kvs["lifetime_seconds"] = "1800";

    QMetaObject::invokeMethod(m_model, [this, kvs]() {
        static int idx = 10000;
        m_model->spotModel().applySpotStatus(idx++, kvs);
    }, Qt::QueuedConnection);

    return {};
}

QString TciProtocol::cmdSpotDelete(const QStringList& args)
{
    if (!m_model || args.size() < 2) return {};
    QString callsign = args[0].trimmed();
    bool ok;
    double freqHz = args[1].toDouble(&ok);
    if (!ok) return {};
    double freqMhz = freqHz / 1e6;

    QMetaObject::invokeMethod(m_model, [this, callsign, freqMhz]() {
        auto& sm = m_model->spotModel();
        for (auto it = sm.spots().begin(); it != sm.spots().end(); ++it) {
            if (it->callsign == callsign &&
                std::abs(it->rxFreqMhz - freqMhz) < 0.0001) {
                sm.removeSpot(it.key());
                break;
            }
        }
    }, Qt::QueuedConnection);

    return {};
}

QString TciProtocol::cmdSpotClear()
{
    if (!m_model) return {};
    QMetaObject::invokeMethod(m_model, [this]() {
        m_model->spotModel().clear();
    }, Qt::QueuedConnection);
    return {};
}

// ── CW macros ──────────────────────────────────────────────────────────────

QString TciProtocol::cmdCwMacros(const QStringList& args)
{
    if (!m_model || args.isEmpty()) return {};
    QString text = args.join(',');
    if (text.isEmpty()) return {};
    QString cmd = QStringLiteral("cwx send \"%1\"").arg(text);
    QMetaObject::invokeMethod(m_model, [this, cmd]() {
        m_model->sendCmdPublic(cmd, nullptr);
    }, Qt::QueuedConnection);
    return {};
}

QString TciProtocol::cmdCwMacrosStop()
{
    if (!m_model) return {};
    QMetaObject::invokeMethod(m_model, [this]() {
        m_model->sendCmdPublic("cwx clear", nullptr);
    }, Qt::QueuedConnection);
    return {};
}

// ── IQ streaming ───────────────────────────────────────────────────────────

QString TciProtocol::cmdIqStart(const QStringList& args)
{
    if (!m_model || args.isEmpty()) return {};
    int channel = args[0].toInt() + 1;  // TRX 0 → DAX IQ channel 1
    if (channel < 1 || channel > 4) return {};
    QMetaObject::invokeMethod(m_model, [this, channel]() {
        m_model->daxIqModel().createStream(channel);
    }, Qt::QueuedConnection);
    return {};
}

QString TciProtocol::cmdIqStop(const QStringList& args)
{
    if (!m_model || args.isEmpty()) return {};
    int channel = args[0].toInt() + 1;
    if (channel < 1 || channel > 4) return {};
    QMetaObject::invokeMethod(m_model, [this, channel]() {
        m_model->daxIqModel().removeStream(channel);
    }, Qt::QueuedConnection);
    return {};
}

QString TciProtocol::cmdIqSampleRate(const QStringList& args, bool isSet)
{
    if (!isSet) {
        if (m_model) {
            int rate = m_model->daxIqModel().stream(1).sampleRate;
            return QStringLiteral("iq_samplerate:%1;").arg(rate);
        }
        return QStringLiteral("iq_samplerate:48000;");
    }

    if (args.isEmpty()) return {};
    int rate = args[0].toInt();
    if (rate != 24000 && rate != 48000 && rate != 96000 && rate != 192000)
        return {};
    if (m_model) {
        QMetaObject::invokeMethod(m_model, [this, rate]() {
            m_model->daxIqModel().setSampleRate(1, rate);
        }, Qt::QueuedConnection);
    }
    m_pendingNotification = QStringLiteral("iq_samplerate:%1;").arg(rate);
    return {};
}

// ── CW keyer (straight key via TCI) ────────────────────────────────────────

QString TciProtocol::cmdKeyer(const QStringList& args)
{
    // keyer:trx,state[,duration_ms]
    // state: true=key down, false=key up
    if (!m_model || args.size() < 2) return {};
    bool down = (args[1].toLower() == "true");
    QMetaObject::invokeMethod(m_model, [this, down]() {
        m_model->sendCwKey(down);
    }, Qt::QueuedConnection);
    return {};
}

QString TciProtocol::cmdCwKeyerSpeed(const QStringList& args, bool isSet)
{
    if (!isSet) {
        int wpm = m_model ? m_model->transmitModel().cwSpeed() : 20;
        return QStringLiteral("cw_keyer_speed:%1;").arg(wpm);
    }
    if (args.isEmpty()) return {};
    int wpm = args[0].toInt();
    if (wpm < 5 || wpm > 100) return {};
    QString cmd = QStringLiteral("cw wpm %1").arg(wpm);
    QMetaObject::invokeMethod(m_model, [this, cmd]() {
        m_model->sendCmdPublic(cmd, nullptr);
    }, Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("cw_keyer_speed:%1;").arg(wpm);
    return {};
}

QString TciProtocol::cmdCwMacrosDelay(const QStringList& args, bool isSet)
{
    // Delay before CW TX starts (ms). FlexRadio has cw_delay.
    static int delay = 0;
    if (!isSet)
        return QStringLiteral("cw_macros_delay:%1;").arg(delay);
    if (args.isEmpty()) return {};
    delay = args[0].toInt();
    if (m_model) {
        QString cmd = QStringLiteral("cw delay %1").arg(delay);
        QMetaObject::invokeMethod(m_model, [this, cmd]() {
            m_model->sendCmdPublic(cmd, nullptr);
        }, Qt::QueuedConnection);
    }
    m_pendingNotification = QStringLiteral("cw_macros_delay:%1;").arg(delay);
    return {};
}

QString TciProtocol::cmdCwTerminal(const QStringList& args, bool isSet)
{
    static bool terminal = false;
    if (!isSet)
        return QStringLiteral("cw_terminal:%1;").arg(terminal ? "true" : "false");
    if (!args.isEmpty())
        terminal = (args[0].toLower() == "true");
    return {};
}

// ── DDS / IF ───────────────────────────────────────────────────────────────

QString TciProtocol::cmdDds(const QStringList& args, bool isSet)
{
    // DDS = center frequency of the receiver (panadapter center)
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        long long hz = static_cast<long long>(std::round(s->frequency() * 1e6));
        return QStringLiteral("dds:%1,%2;").arg(trx).arg(hz);
    }

    if (args.size() < 2) return {};
    // DDS set changes the panadapter center, not the VFO — acknowledge only
    return {};
}

QString TciProtocol::cmdIf(const QStringList& args, bool isSet)
{
    // IF offset — RIT equivalent in TCI
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("if:%1,0,%2;")
                   .arg(trx).arg(s->ritOn() ? s->ritFreq() : 0);
    }

    if (args.size() < 3) return {};
    int offset = args[2].toInt();
    bool on = (offset != 0);
    QMetaObject::invokeMethod(s, [s, on, offset]() { s->setRit(on, offset); },
                              Qt::QueuedConnection);
    return {};
}

// ── Per-receiver audio ─────────────────────────────────────────────────────

QString TciProtocol::cmdRxChannelEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    if (!isSet)
        return QStringLiteral("rx_channel_enable:%1,true;").arg(trx);
    // Always enabled — FlexRadio slices are always active
    return {};
}

QString TciProtocol::cmdRxVolume(const QStringList& args, bool isSet)
{
    // Per-receiver volume — maps to slice audioGain
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_volume:%1,%2;")
                   .arg(trx).arg(static_cast<int>(s->audioGain()));
    }

    if (args.size() < 2) return {};
    float gain = static_cast<float>(args[1].toInt());
    QMetaObject::invokeMethod(s, [s, gain]() { s->setAudioGain(gain); },
                              Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("rx_volume:%1,%2;")
                                .arg(trx).arg(static_cast<int>(gain));
    return {};
}

QString TciProtocol::cmdRxMute(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_mute:%1,%2;")
                   .arg(trx).arg(s->audioMute() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool mute = (args[1].toLower() == "true");
    QMetaObject::invokeMethod(s, [s, mute]() { s->setAudioMute(mute); },
                              Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("rx_mute:%1,%2;")
                                .arg(trx).arg(mute ? "true" : "false");
    return {};
}

QString TciProtocol::cmdRxBalance(const QStringList& args, bool isSet)
{
    // RX audio balance — maps to slice audioPan (0=left, 50=center, 100=right)
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        // TCI balance: -40 to +40 (0=center)
        // FlexRadio: 0–100 (50=center)
        int tciBalance = s->audioPan() - 50;
        return QStringLiteral("rx_balance:%1,%2;").arg(trx).arg(tciBalance);
    }

    if (args.size() < 2) return {};
    int tciBalance = args[1].toInt();  // -40 to +40
    int flexPan = qBound(0, tciBalance + 50, 100);
    QMetaObject::invokeMethod(s, [s, flexPan]() { s->setAudioPan(flexPan); },
                              Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("rx_balance:%1,%2;")
                                .arg(trx).arg(tciBalance);
    return {};
}

// ── Monitor ────────────────────────────────────────────────────────────────

QString TciProtocol::cmdMonEnable(const QStringList& args, bool isSet)
{
    if (!m_model) return {};
    if (!isSet) {
        return QStringLiteral("mon_enable:%1;")
                   .arg(m_model->transmitModel().sbMonitor() ? "true" : "false");
    }
    if (args.isEmpty()) return {};
    bool on = (args[0].toLower() == "true");
    QMetaObject::invokeMethod(m_model, [this, on]() {
        m_model->transmitModel().setSbMonitor(on);
    }, Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("mon_enable:%1;")
                                .arg(on ? "true" : "false");
    return {};
}

QString TciProtocol::cmdMonVolume(const QStringList& args, bool isSet)
{
    if (!m_model) return {};
    if (!isSet) {
        return QStringLiteral("mon_volume:%1;")
                   .arg(m_model->transmitModel().monGainSb());
    }
    if (args.isEmpty()) return {};
    int vol = args[0].toInt();
    QMetaObject::invokeMethod(m_model, [this, vol]() {
        m_model->transmitModel().setMonGainSb(vol);
    }, Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("mon_volume:%1;").arg(vol);
    return {};
}

// ── DSP parameters ─────────────────────────────────────────────────────────

QString TciProtocol::cmdRxNbParam(const QStringList& args, bool isSet)
{
    // NB parameter — maps to slice nbLevel (0–100)
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_nb_param:%1,0,%2;")
                   .arg(trx).arg(s->nbLevel());
    }

    if (args.size() < 3) return {};
    int level = args[2].toInt();
    QMetaObject::invokeMethod(s, [s, level]() { s->setNbLevel(level); },
                              Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("rx_nb_param:%1,0,%2;")
                                .arg(trx).arg(level);
    return {};
}

QString TciProtocol::cmdRxBinEnable(const QStringList& args, bool isSet)
{
    // Binaural — no direct FlexRadio equivalent, acknowledge only
    static bool binEnabled = false;
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    if (!isSet)
        return QStringLiteral("rx_bin_enable:%1,%2;")
                   .arg(trx).arg(binEnabled ? "true" : "false");
    if (args.size() < 2) return {};
    binEnabled = (args[1].toLower() == "true");
    return {};
}

QString TciProtocol::cmdRxAncEnable(const QStringList& args, bool isSet)
{
    // Adaptive Noise Cancellation — no direct FlexRadio equivalent
    static bool ancEnabled = false;
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    if (!isSet)
        return QStringLiteral("rx_anc_enable:%1,%2;")
                   .arg(trx).arg(ancEnabled ? "true" : "false");
    if (args.size() < 2) return {};
    ancEnabled = (args[1].toLower() == "true");
    return {};
}

QString TciProtocol::cmdRxDseEnable(const QStringList& args, bool isSet)
{
    // Digital Surround Effect — no direct FlexRadio equivalent
    static bool dseEnabled = false;
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    if (!isSet)
        return QStringLiteral("rx_dse_enable:%1,%2;")
                   .arg(trx).arg(dseEnabled ? "true" : "false");
    if (args.size() < 2) return {};
    dseEnabled = (args[1].toLower() == "true");
    return {};
}

QString TciProtocol::cmdRxNfEnable(const QStringList& args, bool isSet)
{
    // Notch filter module — maps to TNF global enable
    // For now, acknowledge only (TNF is per-notch, not a global toggle in the same way)
    static bool nfEnabled = true;
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    if (!isSet)
        return QStringLiteral("rx_nf_enable:%1,%2;")
                   .arg(trx).arg(nfEnabled ? "true" : "false");
    if (args.size() < 2) return {};
    nfEnabled = (args[1].toLower() == "true");
    return {};
}

// ── Digital mode offsets ───────────────────────────────────────────────────

QString TciProtocol::cmdDiglOffset(const QStringList& args, bool isSet)
{
    if (!m_model) return {};
    auto* s = sliceForTrx(0);
    if (!s) return {};

    if (!isSet)
        return QStringLiteral("digl_offset:%1;").arg(s->diglOffset());

    if (args.isEmpty()) return {};
    int hz = args[0].toInt();
    QMetaObject::invokeMethod(s, [s, hz]() { s->setDiglOffset(hz); },
                              Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("digl_offset:%1;").arg(hz);
    return {};
}

QString TciProtocol::cmdDiguOffset(const QStringList& args, bool isSet)
{
    if (!m_model) return {};
    auto* s = sliceForTrx(0);
    if (!s) return {};

    if (!isSet)
        return QStringLiteral("digu_offset:%1;").arg(s->diguOffset());

    if (args.isEmpty()) return {};
    int hz = args[0].toInt();
    QMetaObject::invokeMethod(s, [s, hz]() { s->setDiguOffset(hz); },
                              Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("digu_offset:%1;").arg(hz);
    return {};
}

// ── Focus / TX frequency ───────────────────────────────────────────────────

QString TciProtocol::cmdSetInFocus()
{
    // Client requests us to raise our window — emit via signal if needed
    // For now, just acknowledge
    return {};
}

QString TciProtocol::cmdTxFrequency()
{
    if (!m_model) return {};
    // Find TX slice frequency
    for (auto* s : m_model->slices()) {
        if (s->isTxSlice()) {
            long long hz = static_cast<long long>(std::round(s->frequency() * 1e6));
            return QStringLiteral("tx_frequency:%1;").arg(hz);
        }
    }
    return {};
}

} // namespace AetherSDR

#endif // HAVE_WEBSOCKETS
