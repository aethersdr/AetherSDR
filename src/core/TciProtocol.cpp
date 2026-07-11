#ifdef HAVE_WEBSOCKETS
#include "TciProtocol.h"
#include "AppSettings.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/PanadapterModel.h"
#include "models/TransmitModel.h"
#include "models/EqualizerModel.h"
#include "models/SpotModel.h"
#include "models/DaxIqModel.h"

#include <QList>
#include <QMetaObject>
#include <algorithm>
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

long long TciProtocol::mhzToHz(double mhz)
{
    return static_cast<long long>(std::round(mhz * 1e6));
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
        {"FDVU", "digu"},  {"FDVL", "digl"},
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

// DDS = the IQ-stream center frequency a skimmer (CW Skimmer / SDC) decodes
// against. On FlexRadio the DAX IQ stream is a *panadapter* stream centered on
// the pan, not the slice (FlexLib: DAXIQChannel is a Panadapter property), so
// the center is the panadapter center, not the slice/VFO frequency. Reporting
// the slice freq mis-maps every detected signal by (slice − panCenter) Hz.
// Falls back to the slice frequency if the pan can't be resolved or has not
// received a normalized center update yet. (#3910, #3913 review)
long long TciProtocol::ddsCenterHz(RadioModel* model, const SliceModel* slice)
{
    if (!slice) {
        return 0;
    }
    if (model) {
        PanadapterModel* pan = model->panadapter(slice->panId());
        if (pan && pan->centerKnown()) {
            return mhzToHz(pan->centerMhz());
        }
    }
    return mhzToHz(slice->frequency());
}

// ── Init burst ─────────────────────────────────────────────────────────────

QString TciProtocol::generateInitBurst()
{
    QString burst;

    // ── Phase 1: Initialization commands (spec section 4.1) ───────────
    // Sent in spec order.  READY is NOT sent here: real ExpertSDR3
    // transmits its complete initial state (including the audio/IQ
    // stream parameters) and only then sends READY, and clients written
    // against it (SDC / CW Skimmer, reported by UT4LW) latch their
    // cached settings when READY arrives — an early READY makes them
    // initialize from defaults (notably a wrong iq_samplerate).  The
    // earlier early-READY rationale from #2597 ("strict clients reject
    // non-init commands before READY") does not match the reference
    // parser it cited: eesdr-tci aborts only on *unrecognized* command
    // names, anywhere in the stream, and imposes no pre-READY grammar;
    // the RF2K-S engagement fix was split_enable, not READY placement.
    burst += QStringLiteral("vfo_limits:1000,75000000;");
    burst += QStringLiteral("if_limits:-48000,48000;");

    const auto slices = m_model ? m_model->slices() : QList<SliceModel*>{};
    int trxCount = slices.size();
    if (trxCount < 1) trxCount = 1;
    burst += QStringLiteral("trx_count:%1;").arg(trxCount);
    // `channels_count` (plural).  The TCI Protocol PDF spec lists this
    // as `CHANNEL_COUNT` (singular), but the reference implementation
    // (ars-ka0s/eesdr-tci on PyPI, used as the basis for many TCI
    // clients including the RF2K-S amplifier firmware) only recognises
    // the plural form — a singular-form command raises ValueError on
    // the client and aborts handshake parsing.  Implementation wins
    // over the PDF.
    burst += QStringLiteral("channels_count:1;");

    // Identity chosen to bypass WSJT-X's TCI gain-reduction code path.
    // WSJT-X's TCITransceiver halves TX sample amplitude (K2 = 0.499/0x7FFF
    // vs K1 = 0.999/0x7FFF, ~-6 dB) when BOTH conditions hold:
    //   device  == "SunSDR2DX" or "SunSDR2PRO"
    //   protocol != starts-with "ExpertSDR3"
    // Either branch alone keeps the K1 (full-amplitude) path; both branches
    // satisfied makes it doubly safe.
    //   - device: literal "AetherSDR" avoids the SunSDR-specific gain trap
    //     AND avoids the leading-space bug in the older "<name> <model>"
    //     form when the radio's nickname is empty.
    //   - protocol: "ExpertSDR3,1.5" sets WSJT-X's ESDR3 flag, which both
    //     gates the gain reduction off and selects WSJT-X command formats
    //     that AetherSDR already handles correctly (proven in v26.5.1).
    // RF2K-S amp whitelist (which keyed on SunSDR2DX + ExpertSDR2) is
    // regressed by this change; a configurable / adaptive identity is
    // tracked in #2806.  Everything else from #2597 (init-burst order,
    // vfo_limits, if_limits, channels_count, split_enable) is preserved
    // and is what the RF2K-S TCI parser actually needs to engage.
    burst += QStringLiteral("device:AetherSDR;");
    burst += QStringLiteral("receive_only:false;");
    burst += QStringLiteral("modulations_list:usb,lsb,cw,cwr,am,sam,fm,nfm,digu,digl,rtty;");
    burst += QStringLiteral("protocol:ExpertSDR3,1.5;");

    // ── Phase 2: Current state notifications ──────────────────────────
    // Identical in syntax to the events fired by a live tuning
    // operation; clients cache them whether they arrive before or after
    // READY (verified in WSJT-X TCITransceiver.cpp, which parses every
    // command on arrival and uses READY only to advance its own init
    // state machine).
    if (m_model) {
        for (auto* s : slices) {
            int trx = tciTrxForSlice(m_model, s);
            const long long hz = mhzToHz(s->frequency());
            burst += QStringLiteral("vfo:%1,0,%2;").arg(trx).arg(hz);
            // dds: = IQ-stream center (panadapter center). Skimmers learn the
            // IQ center only from this; without it every spot is mis-mapped by
            // (slice − panCenter) Hz. (#3910)
            burst += QStringLiteral("dds:%1,%2;").arg(trx).arg(ddsCenterHz(m_model, s));
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

            // Split — explicitly false so single-VFO operation is signalled.
            // The RF2K-S TCI client uses `split_enable:0,false;` as the
            // signal that VFO 0 is the active VFO; without ever receiving
            // it, its `currentPosition` stays None and get_frequency()
            // returns None, causing the amp to fall back to UNIV and show
            // "No TCI available" regardless of how many `vfo:` events it
            // receives.  (Source: rf-kit-gui_v198/operational_interface/
            // tciSupport.py, SplitThreadSafeValue.get() + extract_current_vfo.)
            burst += QStringLiteral("split_enable:%1,false;").arg(trx);

            // Lock
            burst += QStringLiteral("lock:%1,%2;")
                         .arg(trx).arg(s->isLocked() ? "true" : "false");

            // SQL
            burst += QStringLiteral("sql_enable:%1,%2;")
                         .arg(trx)
                         .arg(s->receiveSquelchOn() ? "true" : "false");
            burst += QStringLiteral("sql_level:%1,%2;")
                         .arg(trx).arg(s->receiveSquelchLevel());

            // AGC
            burst += QStringLiteral("agc_mode:%1,%2;")
                         .arg(trx).arg(s->receiveAgcMode().toLower());

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
        burst += QStringLiteral("mic_level:%1;").arg(tx.micLevel());
        burst += QStringLiteral("trx:%1,%2;").arg(txTrx).arg(isTx ? "true" : "false");

        // Master AF volume — whole-radio (no trx prefix), same saved value
        // cmdVolume's GET returns, reported in dB per the TCI spec
        // (-60..0; ExpertSDR3 wire scale). Without it the init burst seeds
        // drive/mic_level but not AF, so a client's local mirror starts at
        // a default guess and the first AF-gain step jumps to that guess
        // instead of the radio's real level (Ulanzi/Elgato/StreamController
        // gain steppers).
        burst += QStringLiteral("volume:%1;")
                     .arg(volumeDbFromPercent(
                         AppSettings::instance().value("MasterVolume", "100").toInt()));
    }

    // ── Phase 3: Audio / IQ stream configuration ──────────────────────
    // Last of the settings: clients-of-audio concerns (WSJT-X, CW
    // Skimmer / SDC), irrelevant to control-only clients like
    // amplifiers — which simply cache or ignore them pre-READY.
    burst += QStringLiteral("audio_samplerate:48000;");
    burst += QStringLiteral("audio_stream_sample_type:float32;");
    burst += QStringLiteral("audio_stream_channels:2;");
    burst += QStringLiteral("audio_stream_samples:2048;");
    burst += QStringLiteral("tx_stream_audio_buffering:50;");
    burst += QStringLiteral("iq_samplerate:48000;");

    // READY terminates the settings dump — it must follow EVERY setting
    // (in particular iq_samplerate), because SDC / CW Skimmer reads its
    // cached settings the moment READY arrives.  Matches real
    // ExpertSDR3 behavior and the spec's READY definition ("sent after
    // the initialization commands").  Reported by Yuri UT4LW.
    burst += QStringLiteral("ready;");

    // START is a bidirectional device-state notification. Stream lifecycle
    // commands are client-owned: AUDIO_START and IQ_START require a receiver
    // argument and are sent by the client after READY. Emitting the old
    // argument-less audio_start primer here wedged SDC before it processed
    // START, so its TCI connection never became active and it never requested
    // the IQ stream used by the CW skimmer (#3913 test-build finding).
    burst += QStringLiteral("start;");

    return burst;
}

// ── Command dispatch ───────────────────────────────────────────────────────

QString TciProtocol::handleCommand(const QString& cmd)
{
    m_pendingNotification.clear();
    m_pendingMasterVolume = -1;
    m_pendingTxGain = -1;

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
    if (name == "mic_level")        return cmdMicLevel(args, isSet);
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
    if (name == "tx_gain")          return cmdTxGain(args, isSet);

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

    // Same policy as RigctlProtocol::cmdSetFreq: recenter only for tunes
    // that leave the pan's current span (band changes); in-span retunes
    // (Doppler steps from satellite trackers) use autopan=0 so the pan
    // stays put.
    RadioModel* model = m_model;
    QMetaObject::invokeMethod(s, [s, model, mhz]() {
        bool inSpan = false;
        if (auto* pan = model ? model->panadapter(s->panId()) : nullptr) {
            const double halfBw = pan->bandwidthMhz() / 2.0;
            inSpan = halfBw > 0.0 && qAbs(mhz - pan->centerMhz()) <= halfBw;
        }
        if (inSpan) s->setFrequency(mhz);
        else        s->tuneAndRecenter(mhz);
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
    // Capture m_model by value, never `this`: this queued lambda can outlive
    // the TciProtocol, which TciServer deletes synchronously when the client
    // disconnects (#2814 use-after-free). Resolve the slice up front too.
    auto* txSlice = tx ? sliceForTrx(trx) : nullptr;
    QMetaObject::invokeMethod(m_model, [model = m_model, tx, txSlice]() {
        // Assign TX to this TRX's slice before keying
        if (tx && txSlice && !txSlice->isTxSlice())
            txSlice->setTxSlice(true);
        model->setTransmit(tx, TransmitModel::PttSource::Dax);
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
    QMetaObject::invokeMethod(m_model, [model = m_model, tune]() {
        if (tune)
            model->transmitModel().startTune(TransmitModel::PttSource::Dax);
        else
            model->transmitModel().stopTune();
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
    QMetaObject::invokeMethod(m_model, [model = m_model, pwr]() {
        model->transmitModel().setRfPower(pwr);
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
    QMetaObject::invokeMethod(m_model, [model = m_model, pwr]() {
        model->transmitModel().setTunePower(pwr);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("tune_drive:%1;").arg(pwr);
    return {};
}

// ── MIC_LEVEL: get/set mic input gain ─────────────────────────────────────

// mic_level bridges TransmitModel::setMicLevel() (the existing radio-side
// setter wired into FlexLib via commandReady) to TCI clients.  Single arg
// 0-100 (percent), global — mic is whole-radio, not per-trx — matching the
// drive/tune_drive convention.  Accepts the legacy TRX-prefixed form
// `mic_level:0,N;` for forward compatibility with strict-spec clients (the
// trx index is ignored since mic is global).
//
// Added 2026-05-27 to unblock the aethersdr-ulanzi-plugin Mic Gain ▲▼ keys,
// which already send the verb but currently land on a missing dispatcher
// entry → silently ignored.
QString TciProtocol::cmdMicLevel(const QStringList& args, bool /*isSet*/)
{
    if (args.isEmpty()) {
        int lvl = m_model ? m_model->transmitModel().micLevel() : 0;
        return QStringLiteral("mic_level:%1;").arg(lvl);
    }
    bool ok;
    int lvl = args[args.size() == 1 ? 0 : 1].toInt(&ok);
    if (!ok) return {};
    QMetaObject::invokeMethod(m_model, [model = m_model, lvl]() {
        model->transmitModel().setMicLevel(lvl);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("mic_level:%1;").arg(lvl);
    return {};
}

// ── TX_GAIN: get/set TCI TX audio gain (AetherSDR extension) ────────────────

// tx_gain is an AetherSDR extension — not in the TCI v2.0 spec. It controls
// TciServer's outbound TX gain (m_txGain), applied to WSJT-X/JTDX audio before
// the radio. Global command, 0-100 (percent of unity), matching the
// drive/tune_drive convention. Like cmdVolume, TciProtocol cannot reach
// TciServer directly, so a SET stashes the value in m_pendingTxGain and
// TciServer applies it via setTxGain() after handleCommand() returns.
QString TciProtocol::cmdTxGain(const QStringList& args, bool /*isSet*/)
{
    if (args.isEmpty()) {
        // GET — current TCI TX gain (0-100). TciServer persists the 0.0-1.0
        // value to TciTxGain whenever it changes.
        float g = AppSettings::instance().value("TciTxGain", "1.00").toFloat();
        return QStringLiteral("tx_gain:%1;").arg(qBound(0, qRound(g * 100.0f), 100));
    }
    int pct = qBound(0, args[args.size() == 1 ? 0 : 1].toInt(), 100);
    m_pendingTxGain = pct;
    m_pendingNotification = QStringLiteral("tx_gain:%1;").arg(pct);
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
    QMetaObject::invokeMethod(m_model, [model = m_model, cmd]() {
        model->sendCmdPublic(cmd, nullptr);
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
    QMetaObject::invokeMethod(m_model, [model = m_model, cmd]() {
        model->sendCmdPublic(cmd, nullptr);
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
                   .arg(trx)
                   .arg(s->receiveSquelchOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = (args[1].toLower() == "true");
    int level = s->receiveSquelchLevel();
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
                   .arg(trx).arg(s->receiveSquelchLevel());
    }

    if (args.size() < 2) return {};
    int level = args[1].toInt();
    bool on = s->receiveSquelchOn();
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
//             SET = `volume:N;` (1 arg, dB, -60..0; -60 = silence)
//
// Wire scale is dB per TCI Protocol v2.0 ("The range of values is from
// -60 to 0 dB, at a value of -60 dB there is no sound") — real
// ExpertSDR3 sends e.g. `VOLUME:-12;`.  Internally AetherSDR's master
// volume is a 0-100 percent amplitude (title bar slider /
// applyMasterVolume), so the dB<->percent conversion happens here at
// the wire and everything inboard stays percent.
//
// Legacy compat: values >= 1 cannot be dB (the spec range is
// non-positive), so they are accepted as the old AetherSDR percent
// scale — existing percent senders keep working.  `volume:0` is 0 dB =
// full volume per spec (mute belongs to the `mute:` command).
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

int TciProtocol::volumeDbFromPercent(int pct)
{
    if (pct <= 0) return -60;
    if (pct > 100) pct = 100;
    const int db = static_cast<int>(std::lround(20.0 * std::log10(pct / 100.0)));
    return std::clamp(db, -60, 0);
}

int TciProtocol::volumePercentFromDb(double db)
{
    if (db <= -60.0) return 0;
    if (db > 0.0) db = 0.0;
    const int pct = static_cast<int>(std::lround(100.0 * std::pow(10.0, db / 20.0)));
    // Integer percent can't resolve below -40 dB; floor at 1 so only
    // -60 dB (the spec's explicit silence point) maps to mute.
    return std::clamp(pct, 1, 100);
}

QString TciProtocol::cmdVolume(const QStringList& args, bool /*isSet*/)
{
    if (args.isEmpty()) {
        // GET — current master volume from saved settings (the same value
        // the title bar slider reads on startup), reported in dB.
        int pct = AppSettings::instance()
                      .value("MasterVolume", "100").toInt();
        return QStringLiteral("volume:%1;").arg(volumeDbFromPercent(pct));
    }

    // SET — accept either spec form (1 arg) or legacy trx-prefixed (2+ args).
    const double val = args[args.size() == 1 ? 0 : 1].toDouble();
    const int pct = (val >= 1.0)
        ? std::min(static_cast<int>(std::lround(val)), 100)  // legacy percent
        : volumePercentFromDb(val);                          // spec dB
    m_pendingMasterVolume = pct;

    // Echo in dB (round-tripped through the percent store so the echo
    // matches what a subsequent GET would return).
    m_pendingNotification =
        QStringLiteral("volume:%1;").arg(volumeDbFromPercent(pct));
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
                   .arg(trx).arg(s->receiveAgcMode().toLower());
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
                   .arg(trx).arg(s->receiveAgcThreshold());
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

    QMetaObject::invokeMethod(m_model, [model = m_model, kvs]() {
        static int idx = 10000;
        model->spotModel().applySpotStatus(idx++, kvs);
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

    QMetaObject::invokeMethod(m_model, [model = m_model, callsign, freqMhz]() {
        auto& sm = model->spotModel();
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
    QMetaObject::invokeMethod(m_model, [model = m_model]() {
        model->spotModel().clear();
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
    QMetaObject::invokeMethod(m_model, [model = m_model, cmd]() {
        model->sendCmdPublic(cmd, nullptr);
    }, Qt::QueuedConnection);
    return {};
}

QString TciProtocol::cmdCwMacrosStop()
{
    if (!m_model) return {};
    QMetaObject::invokeMethod(m_model, [model = m_model]() {
        model->sendCmdPublic("cwx clear", nullptr);
    }, Qt::QueuedConnection);
    return {};
}

// ── IQ streaming ───────────────────────────────────────────────────────────

QString TciProtocol::cmdIqStart(const QStringList& args)
{
    if (!m_model || args.isEmpty()) return {};
    const int trx = args[0].toInt();
    const int channel = trx + 1;  // TRX 0 → DAX IQ channel 1
    if (channel < 1 || channel > 4) return {};
    // Creating the dax_iq stream is not enough to make IQ flow: on FlexRadio
    // `daxiq_channel` is a Panadapter property, so the radio streams nothing
    // (or a stale pan's IQ) until a panadapter is routed to this channel with
    // `display pan set <panId> daxiq_channel=N`. The GUI does this from the
    // spectrum overlay menu, but a TCI skimmer client (SDC / CW Skimmer) has no
    // such hook — so bind the requested TRX's pan here. Mirrors the only other
    // non-GUI IQ consumer, WfmDemodulator, and centers the stream on the same
    // pan whose center is reported to the client via dds: (#3910/#3913).
    const SliceModel* s = sliceForTrx(trx);
    const QString panId = s ? s->panId() : QString();
    QMetaObject::invokeMethod(m_model, [model = m_model, channel, panId]() {
        model->daxIqModel().createStream(channel);
        if (!panId.isEmpty())
            model->sendCommand(QStringLiteral("display pan set %1 daxiq_channel=%2")
                                   .arg(panId).arg(channel));
    }, Qt::QueuedConnection);
    return {};
}

QString TciProtocol::cmdIqStop(const QStringList& args)
{
    if (!m_model || args.isEmpty()) return {};
    int channel = args[0].toInt() + 1;
    if (channel < 1 || channel > 4) return {};
    QMetaObject::invokeMethod(m_model, [model = m_model, channel]() {
        model->daxIqModel().removeStream(channel);
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
    const int rate = args[0].toInt();
    if (rate != 24000 && rate != 48000 && rate != 96000 && rate != 192000) {
        // Reject without hanging the requester: report the rate that remains
        // in force. No pending notification is set because other clients saw
        // no state change (#3913 review).
        const int currentRate = m_model
            ? m_model->daxIqModel().stream(1).sampleRate
            : 48000;
        return QStringLiteral("iq_samplerate:%1;").arg(currentRate);
    }
    if (m_model) {
        QMetaObject::invokeMethod(m_model, [model = m_model, rate]() {
            model->daxIqModel().setSampleRate(1, rate);
        }, Qt::QueuedConnection);
    }
    // Echo the achieved rate to BOTH the requester and other clients. The
    // server is authoritative for the rate (it clamps/rejects above), so a
    // skimmer (CW Skimmer / SDC) blocks waiting for this confirmation. The
    // dispatcher sends the return value to the requesting socket and the
    // pendingNotification to all *other* sockets, so each client gets exactly
    // one copy — without the return, the requester got no reply at all. (#3910)
    m_pendingNotification = QStringLiteral("iq_samplerate:%1;").arg(rate);
    return QStringLiteral("iq_samplerate:%1;").arg(rate);
}

// ── CW keyer (straight key via TCI) ────────────────────────────────────────

QString TciProtocol::cmdKeyer(const QStringList& args)
{
    // keyer:trx,state[,duration_ms]
    // state: true=key down, false=key up
    if (!m_model || args.size() < 2) return {};
    bool down = (args[1].toLower() == "true");
    QMetaObject::invokeMethod(m_model, [model = m_model, down]() {
        model->sendCwKey(down);
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
    QMetaObject::invokeMethod(m_model, [model = m_model, cmd]() {
        model->sendCmdPublic(cmd, nullptr);
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
        QMetaObject::invokeMethod(m_model, [model = m_model, cmd]() {
            model->sendCmdPublic(cmd, nullptr);
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
    // DDS = center frequency of the receiver (panadapter center, not the VFO).
    if (args.isEmpty()) return {};
    int trx = args[0].toInt();
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("dds:%1,%2;").arg(trx).arg(ddsCenterHz(m_model, s));
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
    QMetaObject::invokeMethod(m_model, [model = m_model, on]() {
        model->transmitModel().setSbMonitor(on);
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
    QMetaObject::invokeMethod(m_model, [model = m_model, vol]() {
        model->transmitModel().setMonGainSb(vol);
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
