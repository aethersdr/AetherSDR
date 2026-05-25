#include "RigctlProtocol.h"
#include "LogManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QLocale>
#include <QMetaObject>
#include <QStringList>
#include <QtGlobal>

namespace AetherSDR {

namespace {

// Existing level bits (kept as-is for compatibility — bit 13 is MICGAIN in
// standard Hamlib 4.x but has always been advertised as RFPOWER here; string
// matching drives the protocol, not the mask).
constexpr quint64 kRigLevelRfPower           = (1ULL << 13);
constexpr quint64 kRigLevelKeyspd            = (1ULL << 14);
constexpr quint64 kRigLevelSwr               = (1ULL << 28);
constexpr quint64 kRigLevelRfPowerMeter      = (1ULL << 32);
constexpr quint64 kRigLevelRfPowerMeterWatts = (1ULL << 39);
constexpr qint64  kTxMeterFreshMs            = 1500;

// New level bits using correct Hamlib 4.x CONSTANT_64BIT_FLAG(n) = 1ULL << n
constexpr quint64 kRigLevelVoxDelay     = (1ULL << 2);
constexpr quint64 kRigLevelAf           = (1ULL << 3);
constexpr quint64 kRigLevelRf           = (1ULL << 4);
constexpr quint64 kRigLevelSql          = (1ULL << 5);
constexpr quint64 kRigLevelApf          = (1ULL << 7);
constexpr quint64 kRigLevelNr           = (1ULL << 8);
constexpr quint64 kRigLevelCwPitch      = (1ULL << 11);
constexpr quint64 kRigLevelComp         = (1ULL << 16);
constexpr quint64 kRigLevelBkIndl       = (1ULL << 18);
constexpr quint64 kRigLevelVoxGain      = (1ULL << 21);
constexpr quint64 kRigLevelStrength     = (1ULL << 30);
constexpr quint64 kRigLevelMonitorGain  = (1ULL << 41);
constexpr quint64 kRigLevelNbDepth      = (1ULL << 42);

constexpr quint64 kRigGetLevelMask = kRigLevelRfPower
                                   | kRigLevelKeyspd
                                   | kRigLevelSwr
                                   | kRigLevelRfPowerMeter
                                   | kRigLevelRfPowerMeterWatts
                                   | kRigLevelVoxDelay
                                   | kRigLevelAf
                                   | kRigLevelRf
                                   | kRigLevelSql
                                   | kRigLevelApf
                                   | kRigLevelNr
                                   | kRigLevelCwPitch
                                   | kRigLevelComp
                                   | kRigLevelBkIndl
                                   | kRigLevelVoxGain
                                   | kRigLevelStrength
                                   | kRigLevelMonitorGain
                                   | kRigLevelNbDepth;
constexpr quint64 kRigSetLevelMask = kRigLevelRfPower
                                   | kRigLevelKeyspd
                                   | kRigLevelVoxDelay
                                   | kRigLevelAf
                                   | kRigLevelRf
                                   | kRigLevelSql
                                   | kRigLevelApf
                                   | kRigLevelNr
                                   | kRigLevelCwPitch
                                   | kRigLevelComp
                                   | kRigLevelBkIndl
                                   | kRigLevelVoxGain
                                   | kRigLevelMonitorGain
                                   | kRigLevelNbDepth;

// Hamlib RIG_FUNC_* bitmasks for supported functions (rig.h)
constexpr quint32 kRigFuncNb    = (1 << 1);
constexpr quint32 kRigFuncComp  = (1 << 2);
constexpr quint32 kRigFuncVox   = (1 << 3);
constexpr quint32 kRigFuncFbKin = (1 << 7);
constexpr quint32 kRigFuncAnf   = (1 << 8);
constexpr quint32 kRigFuncNr    = (1 << 9);
constexpr quint32 kRigFuncApf   = (1 << 10);
constexpr quint32 kRigFuncLock  = (1 << 15);
constexpr quint32 kRigFuncMute  = (1 << 16);
constexpr quint32 kRigFuncSql   = (1 << 19);

constexpr quint32 kRigGetFuncMask = kRigFuncNb | kRigFuncComp | kRigFuncVox
                                  | kRigFuncFbKin | kRigFuncAnf | kRigFuncNr
                                  | kRigFuncApf | kRigFuncLock | kRigFuncMute
                                  | kRigFuncSql;
constexpr quint32 kRigSetFuncMask = kRigGetFuncMask;

// S9 reference level on HF (dBm). STRENGTH = sLevel - kS9Dbm.
constexpr double kS9Dbm = -73.0;

// VFO operation bitmask (Hamlib RIG_OP_*)
constexpr quint32 kRigVfoOpUp   = (1 << 5);
constexpr quint32 kRigVfoOpDown = (1 << 6);
constexpr quint32 kRigVfoOpMask = kRigVfoOpUp | kRigVfoOpDown;

QStringList rigGetLevelTokens()
{
    return {
        QStringLiteral("RFPOWER"),     QStringLiteral("KEYSPD"),
        QStringLiteral("SWR"),         QStringLiteral("RFPOWER_METER"),
        QStringLiteral("RFPOWER_METER_WATTS"),
        QStringLiteral("AF"),          QStringLiteral("RF"),
        QStringLiteral("SQL"),         QStringLiteral("APF"),
        QStringLiteral("NR"),          QStringLiteral("NB"),
        QStringLiteral("CWPITCH"),     QStringLiteral("MICGAIN"),
        QStringLiteral("COMP"),        QStringLiteral("VOXGAIN"),
        QStringLiteral("VOXDELAY"),    QStringLiteral("MONITOR_GAIN"),
        QStringLiteral("BKINDL"),      QStringLiteral("STRENGTH"),
    };
}

QStringList rigSetLevelTokens()
{
    return {
        QStringLiteral("RFPOWER"),  QStringLiteral("KEYSPD"),
        QStringLiteral("AF"),       QStringLiteral("RF"),
        QStringLiteral("SQL"),      QStringLiteral("APF"),
        QStringLiteral("NR"),       QStringLiteral("NB"),
        QStringLiteral("CWPITCH"),  QStringLiteral("MICGAIN"),
        QStringLiteral("COMP"),     QStringLiteral("VOXGAIN"),
        QStringLiteral("VOXDELAY"), QStringLiteral("MONITOR_GAIN"),
        QStringLiteral("BKINDL"),
    };
}

QStringList rigGetFuncTokens()
{
    return {
        QStringLiteral("NB"),   QStringLiteral("COMP"), QStringLiteral("VOX"),
        QStringLiteral("FBKIN"),QStringLiteral("ANF"),  QStringLiteral("NR"),
        QStringLiteral("APF"),  QStringLiteral("LOCK"), QStringLiteral("MUTE"),
        QStringLiteral("SQL"),
    };
}

QString formatRigLevelValue(double value)
{
    QString text = QLocale::c().toString(value, 'g', 6);
    if (text == "-0" || text == "-0.0")
        text = "0";
    return text;
}

// Map antenna name (SmartSDR convention) to Hamlib RIG_ANT_* bitmask.
int antNameToMask(const QString& name)
{
    if (name == "ANT1") return 1;
    if (name == "ANT2") return 2;
    if (name == "ANT3") return 4;
    return 8;  // XVTR or any other
}

// Map Hamlib RIG_ANT_* bitmask to SmartSDR antenna name.
QString antMaskToName(int mask, const QStringList& available)
{
    static const QMap<int, QString> kStdNames = {{1,"ANT1"},{2,"ANT2"},{4,"ANT3"},{8,"XVTR"}};
    QString name = kStdNames.value(mask, "ANT1");
    if (available.isEmpty() || available.contains(name))
        return name;
    // If name isn't in the list, pick by bit-position index
    int bit = 0;
    int m = mask;
    while (m > 1) { m >>= 1; bit++; }
    return (bit < available.size()) ? available[bit] : available.first();
}

} // namespace

RigctlProtocol::RigctlProtocol(RadioModel* model)
    : m_model(model)
{}

// ── Mode conversion tables ──────────────────────────────────────────────────
// SmartSDR mode names observed on FLEX-8600 fw v1.4.0.0.
// Hamlib mode names follow rig.h RIG_MODE_* definitions.

QString RigctlProtocol::smartsdrToHamlib(const QString& mode)
{
    // Mapping verified against SmartSDR v4.1.5 / fw v1.4.0.0 mode list.
    static const QMap<QString, QString> map = {
        {"USB",  "USB"},   {"LSB",  "LSB"},
        {"CW",   "CW"},   {"CWL",  "CWR"},
        {"AM",   "AM"},   {"SAM",  "AMS"},
        {"FM",   "FM"},   {"NFM",  "FM"},
        {"DFM",  "FM"},   {"FDM",  "FM"},
        {"DIGU", "PKTUSB"}, {"DIGL", "PKTLSB"},
        {"RTTY", "RTTY"},
    };
    return map.value(mode.toUpper(), "USB");
}

QString RigctlProtocol::hamlibToSmartSDR(const QString& mode)
{
    static const QMap<QString, QString> map = {
        {"USB",    "USB"},   {"LSB",    "LSB"},
        {"CW",     "CW"},   {"CWR",    "CWL"},
        {"AM",     "AM"},   {"AMS",    "SAM"},
        {"FM",     "FM"},   {"WFM",    "FM"},
        {"PKTUSB", "DIGU"}, {"PKTLSB", "DIGL"},
        {"RTTY",   "RTTY"}, {"RTTYR",  "RTTY"},
    };
    return map.value(mode.toUpper(), "USB");
}

// Hamlib mode flag values (from hamlib/rig.h RIG_MODE_*)
int RigctlProtocol::hamlibModeFlag(const QString& mode)
{
    static const QMap<QString, int> map = {
        {"AM",     0x1},    {"CW",     0x2},
        {"USB",    0x4},    {"LSB",    0x8},
        {"RTTY",   0x10},   {"FM",     0x20},
        {"WFM",    0x40},   {"CWR",    0x80},
        {"RTTYR",  0x100},  {"AMS",    0x200},
        {"PKTLSB", 0x400},  {"PKTUSB", 0x800},
        {"PKTFM",  0x1000},
    };
    return map.value(mode, 0x4);
}

// ── Helpers ─────────────────────────────────────────────────────────────────

SliceModel* RigctlProtocol::currentSlice() const
{
    if (!m_model || !m_model->isConnected())
        return nullptr;
    auto slices = m_model->slices();
    for (auto* s : slices) {
        if (s->sliceId() == m_sliceIndex)
            return s;
    }
    // Fallback to first slice
    return slices.isEmpty() ? nullptr : slices.first();
}

QString RigctlProtocol::rprt(int code) const
{
    return QStringLiteral("RPRT %1\n").arg(code);
}

// ── Main entry point ────────────────────────────────────────────────────────

QString RigctlProtocol::handleLine(const QString& line)
{
    const QString trimmedIn = line.trimmed();
    if (!trimmedIn.isEmpty())
        qCDebug(lcCat).noquote() << "rigctld ←" << trimmedIn;
    const QString resp = handleLineImpl(line);
    if (!resp.isEmpty())
        qCDebug(lcCat).noquote() << "rigctld →" << resp.trimmed();
    return resp;
}

QString RigctlProtocol::handleLineImpl(const QString& line)
{
    if (m_pendingMorseLine) {
        m_pendingMorseLine = false;
        return cmdSendMorse(line.trimmed());
    }

    QString trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return {};

    // Check for extended mode prefix
    if (!trimmed.isEmpty() && (trimmed[0] == '+' || trimmed[0] == ';')) {
        if (trimmed[0] == '+') {
            m_extended = true;
            trimmed = trimmed.mid(1);
        }
    }

    // Pipe separator mode: '|' splits commands and implies extended responses
    // joined by '|' instead of newlines (standard rigctld wire protocol).
    const bool pipeMode = trimmed.contains('|');
    if (pipeMode) {
        const bool savedExtended = m_extended;
        m_extended = true;

        QStringList cmds = trimmed.split('|', Qt::SkipEmptyParts);
        QStringList results;
        for (const auto& cmd : cmds) {
            QString r = processCommand(cmd.trimmed());
            // Each response ends with '\n'; strip trailing newline before joining
            if (r.endsWith('\n'))
                r.chop(1);
            // Replace interior newlines with '|' for pipe-mode formatting
            r.replace('\n', '|');
            results << r;
        }
        m_extended = savedExtended;
        return results.join('|') + QChar('\n');
    }

    // Split on ';' for batch commands
    QStringList cmds = trimmed.split(';', Qt::SkipEmptyParts);
    QString response;
    for (const auto& cmd : cmds) {
        response += processCommand(cmd.trimmed());
    }
    return response;
}

// ── Command dispatch ────────────────────────────────────────────────────────

QString RigctlProtocol::processCommand(const QString& cmd)
{
    if (cmd.isEmpty())
        return {};

    // Long form: \command_name [args]
    if (cmd.startsWith('\\')) {
        QString rest = cmd.mid(1);
        int spaceIdx = rest.indexOf(' ');
        QString name = (spaceIdx >= 0) ? rest.left(spaceIdx) : rest;
        QString args = (spaceIdx >= 0) ? rest.mid(spaceIdx + 1).trimmed() : QString();

        if (name == "get_freq")       return cmdGetFreq();
        if (name == "set_freq")       return cmdSetFreq(args);
        if (name == "get_mode")       return cmdGetMode();
        if (name == "set_mode")       return cmdSetMode(args);
        if (name == "get_vfo")        return cmdGetVfo();
        if (name == "set_vfo")        return cmdSetVfo(args);
        if (name == "get_ptt")        return cmdGetPtt();
        if (name == "set_ptt")        return cmdSetPtt(args);
        if (name == "get_info")       return cmdGetInfo();
        if (name == "get_rig_info")   return cmdGetRigInfo();
        if (name == "get_split_vfo")  return cmdGetSplitVfo();
        if (name == "set_split_vfo")  return cmdSetSplitVfo(args);
        if (name == "get_split_freq") return cmdGetSplitFreq();
        if (name == "set_split_freq") return cmdSetSplitFreq(args);
        if (name == "get_split_mode") return cmdGetSplitMode();
        if (name == "set_split_mode") return cmdSetSplitMode(args);
        if (name == "get_level")      return cmdGetLevel(args);
        if (name == "set_level")      return cmdSetLevel(args);
        if (name == "get_func")       return cmdGetFunc(args);
        if (name == "set_func")       return cmdSetFunc(args);
        if (name == "get_rit")        return cmdGetRit();
        if (name == "set_rit")        return cmdSetRit(args);
        if (name == "get_xit")        return cmdGetXit();
        if (name == "set_xit")        return cmdSetXit(args);
        if (name == "get_ant")        return cmdGetAnt();
        if (name == "set_ant")        return cmdSetAnt(args);
        if (name == "get_ts")         return cmdGetTs();
        if (name == "set_ts")         return cmdSetTs(args);
        if (name == "get_dcd")        return cmdGetDcd();
        if (name == "get_trn")        return cmdGetTrn();
        if (name == "set_trn")        return cmdSetTrn(args);
        if (name == "vfo_op")         return cmdVfoOp(args);
        if (name == "get_vfo_info")   return cmdGetVfoInfo(args);
        if (name == "power2mW")       return cmdPower2mW(args);
        if (name == "mW2power")       return cmdMW2power(args);
        if (name == "dump_state")     return cmdDumpState();
        if (name == "quit")           return {};  // caller handles disconnect
        if (name == "chk_vfo") {
            if (m_extended)
                return QStringLiteral("chk_vfo:\nVFO Mode: 0\n") + rprt(0);
            return QStringLiteral("0\n") + rprt(0);
        }
        if (name == "send_morse")     return cmdSendMorse(args);
        if (name == "stop_morse")     return cmdStopMorse();
        if (name == "wait_morse")     return rprt(-4); // CWX queue depth not queryable; can't actually wait
        if (name == "get_powerstat") {
            // Always report power on — AetherSDR is connected by definition.
            if (m_extended)
                return QStringLiteral("get_powerstat:\nPower Status: 1\n") + rprt(0);
            return QStringLiteral("1\n") + rprt(0);
        }
        if (name == "set_powerstat") {
            // "1" = power on: radio is already on, accept as no-op.
            // Anything else (e.g. "0" = power off) is not supported on a
            // network-connected FlexRadio — reject rather than lie.
            return args.trimmed() == "1" ? rprt(0) : rprt(-1);
        }
        if (name == "get_lock_mode") {
            // Lock mode not supported — always report unlocked so clients like
            // WSJT-X 3.0 don't interpret RPRT -4 as lock mode being active and
            // suppress their own mode-set commands.
            if (m_extended)
                return QStringLiteral("get_lock_mode:\nLock Mode: 0\n") + rprt(0);
            // Hamlib's NET rigctl backend probes this long-form command during
            // WSJT-X startup and waits for a status terminator after the value.
            // Without it, the client blocks until its command timeout, delaying
            // the queued QSY after a USB->DIGU mode change.
            return QStringLiteral("0\n") + rprt(0);
        }
        if (name == "set_lock_mode") {
            // "0" = unlock: already unlocked, accept as no-op.
            // "1" = lock: not enforced on a FlexRadio — reject so clients don't
            // believe the VFO/mode is locked when it isn't.
            return args.trimmed() == "0" ? rprt(0) : rprt(-1);
        }

        // Hamlib NET rigctl handshake commands (sent by the Hamlib library itself)
        if (name == "set_vfo_opt")     return rprt(0);   // VFO-prefix mode; we use chk_vfo=0 so this is a no-op
        if (name == "halt")            return {};         // clean shutdown, same as quit
        if (name == "hamlib_version")  {
            if (m_extended)
                return QStringLiteral("hamlib_version:\nHamlib Version: AetherSDR\n") + rprt(0);
            return QStringLiteral("AetherSDR\n") + rprt(0);
        }
        if (name == "client_version")  return rprt(0);   // silently accept client's version announcement

        // Combined split freq+mode getter/setter (Hamlib 4.x convenience commands)
        if (name == "get_split_freq_mode") {
            auto* txSlice = findTxSlice();
            if (!txSlice) return rprt(-1);
            const long long hz = static_cast<long long>(std::round(txSlice->frequency() * 1e6));
            const QString mode = smartsdrToHamlib(txSlice->mode());
            const int passband = qAbs(txSlice->filterHigh() - txSlice->filterLow());
            if (m_extended)
                return QStringLiteral("get_split_freq_mode:\nTX Frequency: %1\nTX Mode: %2\nTX Passband: %3\n")
                           .arg(hz).arg(mode).arg(passband) + rprt(0);
            return QStringLiteral("%1\n%2\n%3\n").arg(hz).arg(mode).arg(passband) + rprt(0);
        }
        if (name == "set_split_freq_mode") {
            // Args: "<freq> <mode> <passband>" — delegate to existing handlers
            const QStringList parts = args.split(' ', Qt::SkipEmptyParts);
            if (parts.size() < 2) return rprt(-1);
            const QString freqResp = cmdSetSplitFreq(parts[0]);
            if (freqResp != rprt(0)) return freqResp;
            return cmdSetSplitMode(parts.mid(1).join(' '));
        }

        // VFO / mode discovery
        if (name == "get_vfo_list") {
            if (m_extended)
                return QStringLiteral("get_vfo_list:\nVFO List: VFOA VFOB\n") + rprt(0);
            return QStringLiteral("VFOA VFOB\n") + rprt(0);
        }
        if (name == "get_modes") {
            static const QString kModes =
                QStringLiteral("USB LSB CW CWR AM AMS FM PKTUSB PKTLSB RTTY");
            if (m_extended)
                return QStringLiteral("get_modes:\nModes: %1\n").arg(kModes) + rprt(0);
            return kModes + "\n" + rprt(0);
        }

        // FM / repeater stubs (not applicable to HF SDR, but prevent -4 noise)
        if (name == "get_rptr_shift") {
            if (m_extended) return QStringLiteral("get_rptr_shift:\nRptr Shift: +\n") + rprt(0);
            return QStringLiteral("+\n") + rprt(0);
        }
        if (name == "set_rptr_shift") return rprt(0);
        if (name == "get_rptr_offs") {
            if (m_extended) return QStringLiteral("get_rptr_offs:\nRptr Offset: 0\n") + rprt(0);
            return QStringLiteral("0\n") + rprt(0);
        }
        if (name == "set_rptr_offs")  return rprt(0);
        if (name == "get_ctcss_tone") {
            if (m_extended) return QStringLiteral("get_ctcss_tone:\nCTCSS Tone: 0\n") + rprt(0);
            return QStringLiteral("0\n") + rprt(0);
        }
        if (name == "set_ctcss_tone") return rprt(0);
        if (name == "get_dcs_code") {
            if (m_extended) return QStringLiteral("get_dcs_code:\nDCS Code: 0\n") + rprt(0);
            return QStringLiteral("0\n") + rprt(0);
        }
        if (name == "set_dcs_code")   return rprt(0);

        // Misc stubs
        if (name == "scan")           return rprt(-11);  // RIG_ENAVAIL
        if (name == "reset")          return rprt(0);
        if (name == "pause")          return rprt(0);
        if (name == "password")       return rprt(0);
        if (name == "dump_caps")      return rprt(-4);   // complex; clients fall back gracefully
        if (name == "dump_conf")      return QStringLiteral("done\n");
        if (name == "get_conf")       return rprt(-11);
        if (name == "set_conf")       return rprt(0);
        if (name == "get_parm")       return rprt(-11);
        if (name == "set_parm")       return rprt(0);
        if (name == "get_clock")      return rprt(-11);
        if (name == "set_clock")      return rprt(0);
        if (name == "send_dtmf")      return rprt(-11);
        if (name == "recv_dtmf")      return rprt(-11);
        if (name == "send_raw")       return rprt(-11);
        if (name == "get_twiddle") {
            if (m_extended) return QStringLiteral("get_twiddle:\nTwiddle Timeout: 0\n") + rprt(0);
            return QStringLiteral("0\n");
        }
        if (name == "set_twiddle")    return rprt(0);
        if (name == "get_cache") {
            if (m_extended) return QStringLiteral("get_cache:\nCache Timeout: 0\n") + rprt(0);
            return QStringLiteral("0\n");
        }
        if (name == "set_cache")      return rprt(0);

        return rprt(-4);  // RIG_EINVAL
    }

    // Short form: single character + optional args
    QChar shortCmd = cmd[0];
    QString args = cmd.mid(1).trimmed();

    // Short-form character assignments from Hamlib tests/rigctl_parse.c (master).
    switch (shortCmd.toLatin1()) {
    // Frequency / mode
    case 'f': return cmdGetFreq();
    case 'F': return cmdSetFreq(args);
    case 'm': return cmdGetMode();
    case 'M': return cmdSetMode(args);
    // VFO
    case 'v': return cmdGetVfo();
    case 'V': return cmdSetVfo(args);
    // PTT
    case 't': return cmdGetPtt();
    case 'T': return cmdSetPtt(args);
    // Split
    case 's': return cmdGetSplitVfo();
    case 'S': return cmdSetSplitVfo(args);
    case 'i': return cmdGetSplitFreq();
    case 'I': return cmdSetSplitFreq(args);
    case 'x': return cmdGetSplitMode();
    case 'X': return cmdSetSplitMode(args);
    case 'k': {                             // get_split_freq_mode
        auto* tx = findTxSlice();
        if (!tx) return rprt(-1);
        const long long hz = static_cast<long long>(std::round(tx->frequency() * 1e6));
        const QString mode = smartsdrToHamlib(tx->mode());
        const int pb = qAbs(tx->filterHigh() - tx->filterLow());
        if (m_extended)
            return QStringLiteral("get_split_freq_mode:\nTX Frequency: %1\nTX Mode: %2\nTX Passband: %3\n")
                       .arg(hz).arg(mode).arg(pb) + rprt(0);
        return QStringLiteral("%1\n%2\n%3\n").arg(hz).arg(mode).arg(pb);
    }
    case 'K': {                             // set_split_freq_mode
        const QStringList parts = args.split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 2) return rprt(-1);
        const QString r1 = cmdSetSplitFreq(parts[0]);
        if (r1 != rprt(0)) return r1;
        return cmdSetSplitMode(parts.mid(1).join(' '));
    }
    // Level / func / parm
    case 'l': return cmdGetLevel(args);
    case 'L': return cmdSetLevel(args);
    case 'u': return cmdGetFunc(args);
    case 'U': return cmdSetFunc(args);
    case 'p': return rprt(-11);             // get_parm — not supported
    case 'P': return rprt(0);              // set_parm — silently accept
    // VFO op / scan
    case 'G': return cmdVfoOp(args);       // vfo_op
    case 'g': return rprt(-11);            // scan — not supported
    // Transceive
    case 'a': return cmdGetTrn();          // get_trn
    case 'A': return cmdSetTrn(args);      // set_trn
    // Repeater / CTCSS / DCS stubs (FM features, not used for HF SDR)
    case 'r': return QStringLiteral("+\n");      // get_rptr_shift
    case 'R': return rprt(0);             // set_rptr_shift
    case 'o': return QStringLiteral("0\n");      // get_rptr_offs
    case 'O': return rprt(0);             // set_rptr_offs
    case 'c': return QStringLiteral("0\n");      // get_ctcss_tone
    case 'C': return rprt(0);             // set_ctcss_tone
    case 'd': return QStringLiteral("0\n");      // get_dcs_code
    case 'D': return rprt(0);             // set_dcs_code
    // RIT / XIT
    case 'j': return cmdGetRit();
    case 'J': return cmdSetRit(args);
    case 'z': return cmdGetXit();
    case 'Z': return cmdSetXit(args);
    // Antenna
    case 'y': return cmdGetAnt();          // get_ant
    case 'Y': return cmdSetAnt(args);      // set_ant
    // Tuning step
    case 'n': return cmdGetTs();           // get_ts
    case 'N': return cmdSetTs(args);       // set_ts
    // Memory / bank / channel stubs
    case 'E': return rprt(-11);            // set_mem
    case 'e': return rprt(-11);            // get_mem
    case 'B': return rprt(-11);            // set_bank
    case 'H': return rprt(-11);            // set_channel
    case 'h': return rprt(-11);            // get_channel
    // Raw / misc
    case 'w': return rprt(-11);            // send_cmd
    case 'W': return rprt(-11);            // send_cmd_rx
    case '*': return rprt(0);              // reset
    // Info / caps
    case '_': return cmdGetInfo();
    case '1': return rprt(-4);             // dump_caps — use \dump_state for state
    case '2': return cmdPower2mW(args);
    case '3': return QStringLiteral("done\n");  // dump_conf
    case '4': return cmdMW2power(args);
    // Morse
    case 'b': return cmdSendMorse(args);
    case 'q': return {};                   // quit
    default:  return rprt(-4);
    }
}

// ── Individual command implementations ──────────────────────────────────────

QString RigctlProtocol::cmdGetFreq()
{
    auto* slice = currentSlice();
    if (!slice) return rprt(-8);  // RIG_ENAVAIL

    auto hz = static_cast<long long>(slice->frequency() * 1e6);
    if (m_extended)
        return QStringLiteral("get_freq:\nFrequency: %1\n").arg(hz) + rprt(0);
    return QStringLiteral("%1\n").arg(hz);
}

QString RigctlProtocol::cmdSetFreq(const QString& arg)
{
    auto* slice = currentSlice();
    if (!slice) return rprt(-8);

    bool ok;
    double hz = arg.toDouble(&ok);
    if (!ok || hz < 0) return rprt(-1);  // RIG_EINVAL

    double mhz = hz / 1e6;
    QMetaObject::invokeMethod(slice, [slice, mhz]() {
        // Use tuneAndRecenter so the panadapter follows cross-band
        // tunes (e.g. WSJT-X band changes). autopan=0 would leave
        // the pan on the old band. (#536)
        slice->tuneAndRecenter(mhz);
    }, Qt::QueuedConnection);
    return rprt(0);
}

QString RigctlProtocol::cmdGetMode()
{
    auto* slice = currentSlice();
    if (!slice) return rprt(-8);

    QString hamlibMode = smartsdrToHamlib(slice->mode());
    int passband = slice->filterHigh() - slice->filterLow();
    if (passband < 0) passband = -passband;

    if (m_extended) {
        return QStringLiteral("get_mode:\nMode: %1\nPassband: %2\n").arg(hamlibMode).arg(passband)
               + rprt(0);
    }
    return QStringLiteral("%1\n%2\n").arg(hamlibMode).arg(passband);
}

QString RigctlProtocol::cmdSetMode(const QString& args)
{
    auto* slice = currentSlice();
    if (!slice) return rprt(-8);

    QStringList parts = args.split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return rprt(-1);

    QString hamlibMode = parts[0].toUpper();

    if (hamlibMode == "?") {
        static const QString kModes =
            QStringLiteral("USB LSB CW CWR AM AMS FM PKTUSB PKTLSB RTTY");
        if (m_extended)
            return QStringLiteral("set_mode:\nModes: %1\nPassbands: 0\n").arg(kModes) + rprt(0);
        return kModes + "\n";
    }

    QString sdrMode = hamlibToSmartSDR(hamlibMode);

    QMetaObject::invokeMethod(slice, [slice, sdrMode]() {
        slice->setMode(sdrMode);
    }, Qt::QueuedConnection);

    // Set passband if provided and > 0. Hamlib's passband is the filter
    // *width* in Hz; SmartSDR's `filt` command takes absolute lo/hi audio
    // edges, and the right placement depends on the mode. Mirrors the
    // canonical mapping in VfoWidget::applyFilterWidthForMode (#2259-era).
    if (parts.size() >= 2) {
        bool ok;
        int passband = parts[1].toInt(&ok);
        if (ok && passband > 0) {
            QMetaObject::invokeMethod(slice, [slice, passband]() {
                const QString m = slice->mode();
                int lo = 95;
                int hi = passband;
                if (m == "CW" || m == "CWL"
                    || m == "AM" || m == "SAM" || m == "DSB"
                    || m == "FM" || m == "NFM" || m == "DFM") {
                    // Symmetric around 0 — CW BFO offset and AM/FM carrier
                    // are at audio DC.
                    lo = -passband / 2;
                    hi =  passband / 2;
                } else if (m == "LSB" || m == "DIGL") {
                    // DIGL audio sits on the lower sideband — same edge
                    // geometry as LSB. (Offset-aware placement for narrow
                    // widths is handled in the GUI path; the rigctld path
                    // uses the wide-fallback geometry from
                    // VfoWidget::applyFilterPreset.)
                    lo = -passband;
                    hi = -95;
                } else {
                    // USB, DIGU, RTTY, FDV, etc. — high-side from 95 Hz
                    lo = 95;
                    hi = passband;
                }
                slice->setFilterWidth(lo, hi);
            }, Qt::QueuedConnection);
        }
    }

    return rprt(0);
}

QString RigctlProtocol::cmdGetVfo()
{
    // Always report VFOA — this connection's slice is the "current" VFO
    // for the client.  The actual slice index is set by the TCP port binding.
    if (m_extended)
        return QStringLiteral("get_vfo:\nVFO: VFOA\n") + rprt(0);
    return QStringLiteral("VFOA\n");
}

QString RigctlProtocol::cmdSetVfo(const QString& arg)
{
    // Accept the command without error but do NOT modify m_sliceIndex.
    // The slice binding is determined by which TCP port the client connected
    // to (set in RigctlServer::onNewConnection) and must not be overridden
    // by the VFO command.  WSJT-X sends "V VFOB" during init which would
    // otherwise force all instances onto Slice B (#1621).
    QString vfo = arg.trimmed().toUpper();
    if (vfo == "VFOA" || vfo == "MAIN" || vfo == "VFOB" || vfo == "SUB")
        return rprt(0);
    return rprt(-1);
}

QString RigctlProtocol::cmdGetPtt()
{
    if (!m_model) return rprt(-8);
    int ptt = m_model->transmitModel().isTransmitting() ? 1 : 0;
    if (m_extended)
        return QStringLiteral("get_ptt:\nPTT: %1\n").arg(ptt) + rprt(0);
    return QStringLiteral("%1\n").arg(ptt);
}

QString RigctlProtocol::cmdSetPtt(const QString& arg)
{
    if (!m_model) return rprt(-8);
    bool ok;
    int ptt = arg.trimmed().toInt(&ok);
    if (!ok) return rprt(-1);

    bool tx = (ptt != 0);
    QMetaObject::invokeMethod(m_model, [model = m_model, sliceId = m_sliceIndex, tx]() {
        // When keying TX, move the TX badge to this protocol's bound slice
        // so the correct slice is used for transmission.
        if (tx && model->isConnected()) {
            const auto slices = model->slices();
            SliceModel* slice = nullptr;
            for (auto* s : slices) {
                if (s->sliceId() == sliceId) { slice = s; break; }
            }
            if (!slice && !slices.isEmpty())
                slice = slices.first();
            if (slice && !slice->isTxSlice())
                slice->setTxSlice(true);
        }
        model->setTransmit(tx, TransmitModel::PttSource::Dax);
    }, Qt::QueuedConnection);
    return rprt(0);
}

QString RigctlProtocol::cmdGetInfo()
{
    if (!m_model || !m_model->isConnected())
        return QStringLiteral("AetherSDR\n");
    return QStringLiteral("%1 %2 v%3\n")
        .arg(m_model->name(), m_model->model(), m_model->version());
}

// Find the TX slice (may differ from the RX slice in split mode)
SliceModel* RigctlProtocol::findTxSlice()
{
    if (!m_model) return nullptr;
    // Promote the new slice if it has appeared since the last check, and apply
    // any stashed split freq/mode from the command burst that preceded it.
    tryPromoteTxSlice();
    // Prefer the pending pointer: setTxSlice(true) may have been queued but not
    // yet processed by the event loop, so isTxSlice() on the target slice is
    // still stale-false.  Returning the intended slice here avoids set_split_freq
    // hitting the old TX slice (still stale-true) in the same command burst.
    if (m_pendingTxSlice) return m_pendingTxSlice;
    for (auto* s : m_model->slices())
        if (s->isTxSlice()) return s;
    return nullptr;
}

// If set_split_vfo 1 arrived when only one slice existed, addSlice() was called
// and this flag was set.  On the next split-related call we scan for the newly
// created slice and promote it to TX so subsequent set_split_freq succeeds.
void RigctlProtocol::tryPromoteTxSlice()
{
    if (!m_pendingSplitEnable || !m_model) return;
    auto* rxSlice = currentSlice();
    for (auto* s : m_model->slices()) {
        if (s != rxSlice) {
            m_pendingSplitEnable = false;
            m_pendingTxSlice = s;
            if (!s->isTxSlice())
                QMetaObject::invokeMethod(s, [s]{ s->setTxSlice(true); },
                                          Qt::QueuedConnection);
            // Apply freq/mode stashed while we waited for this slice to appear.
            if (m_pendingSplitFreqMHz > 0.0) {
                s->setFrequency(m_pendingSplitFreqMHz);
                m_pendingSplitFreqMHz = 0.0;
            }
            if (!m_pendingSplitMode.isEmpty()) {
                s->setMode(m_pendingSplitMode);
                m_pendingSplitMode.clear();
            }
            return;
        }
    }
    // New slice not yet visible — will retry on the next call.
}

QString RigctlProtocol::cmdGetSplitVfo()
{
    tryPromoteTxSlice();
    auto* rxSlice = currentSlice();
    auto* txSlice = findTxSlice();
    bool split = (rxSlice && txSlice && rxSlice != txSlice);
    // Report TX VFO as VFOB when split (TX on a different slice), VFOA otherwise.
    // The actual slice is resolved internally — the VFO label is only for the client.
    const QString txVfo = split ? "VFOB" : "VFOA";
    if (m_extended)
        return QString("get_split_vfo:\nSplit: %1\nTX VFO: %2\n").arg(split ? 1 : 0).arg(txVfo) + rprt(0);
    return QString("%1\n%2\n").arg(split ? 1 : 0).arg(txVfo);
}

QString RigctlProtocol::cmdSetSplitVfo(const QString& args)
{
    // Format: "1 VFOB" or "0 VFOA"
    QStringList parts = args.split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return rprt(-1);
    const bool enable = (parts[0] == "1");

    auto* rxSlice = currentSlice();
    if (!rxSlice) return rprt(-8);

    // Edge-only reclaim: only move TX back to our RX slice on a genuine
    // split→non-split *transition*, never on a steady-state poll.  Hamlib
    // loggers (VARA/VARAC/N1MM) poll `set_split_vfo 0 VFOA` every few
    // seconds; each CAT channel binds to a fixed slice, so reassigning TX
    // to this channel's slice on every poll (the prior !isTxSlice() guard
    // only suppressed the already-TX case) means the moment the user moves
    // TX to another slice, the next poll on a non-TX channel re-seizes it
    // — TX visibly ping-pongs back to this channel's slice within ~2 s.
    // Acting only on the 1→0 edge (or the same-pass race below) lets a
    // logger that never toggles split leave the user's TX choice alone.
    const bool wasEnabled = (m_lastSplitEnable == 1);
    const bool firstReport = (m_lastSplitEnable < 0);
    m_lastSplitEnable = enable ? 1 : 0;

    if (!enable) {
        m_pendingSplitEnable = false;
        m_pendingTxSlice = nullptr;
        m_pendingSplitFreqMHz = 0.0;
        m_pendingSplitMode.clear();

        // m_pendingTxSliceChange catches the race where set_split_vfo 1 and
        // set_split_vfo 0 arrive in the same onClientData() pass: the queued
        // setTxSlice(true) for the TX slice hasn't fired yet, so isTxSlice()
        // on the RX slice is still stale-true. Without this flag the disable
        // command would be a no-op and the other slice would become TX.
        const bool wasPending = m_pendingTxSliceChange;
        m_pendingTxSliceChange = false;

        // Reclaim TX only on a real 1→0 transition or the same-pass race.
        // Steady-state polls (already-disabled split, first-report-after-
        // connect) leave the user's TX badge alone.
        if ((wasEnabled && !firstReport) || wasPending) {
            if (!rxSlice->isTxSlice() || wasPending)
                rxSlice->setTxSlice(true);
        }
        return rprt(0);
    }

    // Enable split: find an existing slice that is not our RX slice and
    // promote it to TX.  If no second slice exists yet, ask the radio to
    // create one; subsequent split-related calls will promote it once the
    // radio's status response has populated the SliceModel.
    for (auto* s : m_model->slices()) {
        if (s != rxSlice) {
            m_pendingSplitEnable = false;
            m_pendingTxSlice = s;
            if (!s->isTxSlice()) {
                m_pendingTxSliceChange = true;
                QMetaObject::invokeMethod(s, [s]{ s->setTxSlice(true); },
                                          Qt::QueuedConnection);
            }
            return rprt(0);
        }
    }

    // No second slice — create one and flag for deferred promotion.
    m_pendingSplitEnable = true;
    auto* model = m_model;
    QMetaObject::invokeMethod(m_model, [model]{ model->addSlice(); },
                              Qt::QueuedConnection);
    return rprt(0);
}

QString RigctlProtocol::cmdGetSplitFreq()
{
    tryPromoteTxSlice();
    auto* txSlice = findTxSlice();
    if (!txSlice) return rprt(-1);
    long long hz = static_cast<long long>(std::round(txSlice->frequency() * 1e6));
    if (m_extended)
        return QString("get_split_freq:\nTX Frequency: %1\n").arg(hz) + rprt(0);
    return QString("%1\n").arg(hz);
}

QString RigctlProtocol::cmdSetSplitFreq(const QString& args)
{
    bool ok;
    double hz = args.trimmed().toDouble(&ok);
    if (!ok) return rprt(-1);
    // If the new slice hasn't appeared yet, stash the freq; tryPromoteTxSlice()
    // will apply it as soon as the slice becomes visible.
    if (m_pendingSplitEnable) {
        m_pendingSplitFreqMHz = hz / 1e6;
        return rprt(0);
    }
    auto* txSlice = findTxSlice();
    if (!txSlice) return rprt(-1);
    txSlice->setFrequency(hz / 1e6);
    return rprt(0);
}

QString RigctlProtocol::cmdGetSplitMode()
{
    auto* txSlice = findTxSlice();
    if (!txSlice) return rprt(-1);
    const QString mode = txSlice->mode();
    // Map FlexRadio mode to Hamlib mode string
    QString hMode = mode;
    if (mode == "DIGU") hMode = "PKTUSB";
    else if (mode == "DIGL") hMode = "PKTLSB";
    else if (mode == "SAM") hMode = "AMS";
    int passband = txSlice->filterHigh() - txSlice->filterLow();
    if (m_extended)
        return QString("get_split_mode:\nTX Mode: %1\nTX Passband: %2\n").arg(hMode).arg(passband) + rprt(0);
    return QString("%1\n%2\n").arg(hMode).arg(passband);
}

QString RigctlProtocol::cmdSetSplitMode(const QString& args)
{
    QStringList parts = args.split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return rprt(-1);
    QString mode = parts[0];
    if (mode == "PKTUSB") mode = "DIGU";
    else if (mode == "PKTLSB") mode = "DIGL";
    else if (mode == "AMS") mode = "SAM";
    // If the new slice hasn't appeared yet, stash the mode for tryPromoteTxSlice().
    if (m_pendingSplitEnable) {
        m_pendingSplitMode = mode;
        return rprt(0);
    }
    auto* txSlice = findTxSlice();
    if (!txSlice) return rprt(-1);
    txSlice->setMode(mode);
    return rprt(0);
}

QString RigctlProtocol::cmdGetLevel(const QString& arg)
{
    if (!m_model) return rprt(-8);

    const QString level = arg.trimmed().toUpper();
    if (level.isEmpty())
        return rprt(-1);

    if (level == "?") {
        const QString supported = rigGetLevelTokens().join(' ');
        if (m_extended)
            return QStringLiteral("get_level:\nLevels: %1\n").arg(supported) + rprt(0);
        return supported + "\n";
    }

    auto makeResponse = [this, &level](const QString& value) {
        if (m_extended) {
            return QStringLiteral("get_level: %1\n%2: %3\n")
                .arg(level, level, value) + rprt(0);
        }
        return value + "\n";
    };

    const auto& txModel = m_model->transmitModel();
    const bool txMetersActive = txModel.isTransmitting() || txModel.isMox() || txModel.isTuning();
    const bool txMetersFresh = txMetersActive
        && m_model->meterModel().hasRecentTxMeters(kTxMeterFreshMs);

    if (level == "KEYSPD")
        return makeResponse(QString::number(txModel.cwSpeed()));

    if (level == "RFPOWER") {
        // Hamlib RIG_LEVEL_RFPOWER is normalized 0.0–1.0; the radio reports
        // 0–100 (percent of max_power_level), so divide by 100.
        const double ratio = qBound(0.0, txModel.rfPower() / 100.0, 1.0);
        return makeResponse(formatRigLevelValue(ratio));
    }

    if (level == "SWR") {
        // WSJT-X polls immediately after PTT and treats 0 as "no valid reading".
        // Suppress cached last-TX values until a fresh TX meter sample arrives.
        const float swr = txMetersFresh ? qMax(0.0f, m_model->meterModel().swr()) : 0.0f;
        return makeResponse(formatRigLevelValue(swr));
    }

    if (level == "RFPOWER_METER_WATTS") {
        const float watts = txMetersFresh ? qMax(0.0f, m_model->meterModel().fwdPower()) : 0.0f;
        return makeResponse(formatRigLevelValue(watts));
    }

    if (level == "RFPOWER_METER") {
        const float watts = txMetersFresh ? qMax(0.0f, m_model->meterModel().fwdPower()) : 0.0f;
        const int maxPower = qMax(1, txModel.maxPowerLevel());
        float ratio = watts / static_cast<float>(maxPower);
        ratio = qBound(0.0f, ratio, 1.0f);
        return makeResponse(formatRigLevelValue(ratio));
    }

    // Slice-dependent levels
    auto* slice = currentSlice();
    if (!slice) return rprt(-8);

    if (level == "AF") {
        const double af = qBound(0.0f, slice->audioGain(), 100.0f) / 100.0;
        return makeResponse(formatRigLevelValue(af));
    }
    if (level == "RF") {
        const double rf = qBound(0.0f, slice->rfGain(), 100.0f) / 100.0;
        return makeResponse(formatRigLevelValue(rf));
    }
    if (level == "SQL") {
        const double sql = qBound(0, slice->squelchLevel(), 100) / 100.0;
        return makeResponse(formatRigLevelValue(sql));
    }
    if (level == "APF") {
        const double apf = qBound(0, slice->apfLevel(), 100) / 100.0;
        return makeResponse(formatRigLevelValue(apf));
    }
    if (level == "NR") {
        const double nr = qBound(0, slice->nrLevel(), 100) / 100.0;
        return makeResponse(formatRigLevelValue(nr));
    }
    if (level == "NB") {
        const double nb = qBound(0, slice->nbLevel(), 100) / 100.0;
        return makeResponse(formatRigLevelValue(nb));
    }
    if (level == "STRENGTH") {
        // S-meter in dBm; STRENGTH is dB relative to S9 (-73 dBm on HF)
        const double strength = m_model->meterModel().sLevel() - kS9Dbm;
        return makeResponse(formatRigLevelValue(strength));
    }

    // TX/radio-wide levels (no slice dependency)
    if (level == "CWPITCH") {
        return makeResponse(QString::number(txModel.cwPitch()));
    }
    if (level == "MICGAIN") {
        const double mic = qBound(0, txModel.micLevel(), 100) / 100.0;
        return makeResponse(formatRigLevelValue(mic));
    }
    if (level == "COMP") {
        const double comp = qBound(0, txModel.speechProcessorLevel(), 100) / 100.0;
        return makeResponse(formatRigLevelValue(comp));
    }
    if (level == "VOXGAIN") {
        const double vox = qBound(0, txModel.voxLevel(), 100) / 100.0;
        return makeResponse(formatRigLevelValue(vox));
    }
    if (level == "VOXDELAY") {
        // raw 0-100 where 1 unit = 20 ms; return in seconds
        const double secs = txModel.voxDelay() * 0.02;
        return makeResponse(formatRigLevelValue(secs));
    }
    if (level == "MONITOR_GAIN") {
        const double mon = qBound(0, txModel.monGainSb(), 100) / 100.0;
        return makeResponse(formatRigLevelValue(mon));
    }
    if (level == "BKINDL") {
        // CW break-in delay is stored in ms; Hamlib expects seconds
        const double secs = txModel.cwDelay() / 1000.0;
        return makeResponse(formatRigLevelValue(secs));
    }

    return rprt(-11);  // RIG_ENAVAIL
}

QString RigctlProtocol::cmdSetLevel(const QString& args)
{
    QStringList parts = args.split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return rprt(-1);

    const QString level = parts[0].trimmed().toUpper();
    if (level == "?") {
        const QString supported = rigSetLevelTokens().join(' ');
        if (m_extended)
            return QStringLiteral("set_level:\nLevels: %1\n").arg(supported) + rprt(0);
        return supported + "\n";
    }
    if (level == "KEYSPD")
        return cmdSetKeySpeed(parts.mid(1).join(' '));

    if (level == "RFPOWER") {
        if (parts.size() < 2) return rprt(-1);
        bool ok = false;
        double ratio = parts[1].toDouble(&ok);
        if (!ok) return rprt(-1);
        // Hamlib delivers RFPOWER as 0.0–1.0; convert to the radio's 0–100
        // scale and let TransmitModel::setRfPower clamp.
        const int percent = qRound(qBound(0.0, ratio, 1.0) * 100.0);
        if (!m_model) return rprt(-8);
        QMetaObject::invokeMethod(m_model, [model = m_model, percent]() {
            model->transmitModel().setRfPower(percent);
        }, Qt::QueuedConnection);
        return rprt(0);
    }

    // All remaining set-levels need a value argument and a connected model
    if (parts.size() < 2) return rprt(-1);
    if (!m_model) return rprt(-8);
    bool ok2 = false;
    double val = parts[1].toDouble(&ok2);
    if (!ok2) return rprt(-1);

    // TX/radio-wide setters
    if (level == "CWPITCH") {
        const int hz = qBound(100, qRound(val), 6000);
        QMetaObject::invokeMethod(m_model, [model = m_model, hz]() {
            model->transmitModel().setCwPitch(hz);
        }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (level == "MICGAIN") {
        const int lvl = qBound(0, qRound(val * 100.0), 100);
        QMetaObject::invokeMethod(m_model, [model = m_model, lvl]() {
            model->transmitModel().setMicLevel(lvl);
        }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (level == "COMP") {
        const int lvl = qBound(0, qRound(val * 100.0), 100);
        QMetaObject::invokeMethod(m_model, [model = m_model, lvl]() {
            model->transmitModel().setSpeechProcessorLevel(lvl);
        }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (level == "VOXGAIN") {
        const int lvl = qBound(0, qRound(val * 100.0), 100);
        QMetaObject::invokeMethod(m_model, [model = m_model, lvl]() {
            model->transmitModel().setVoxLevel(lvl);
        }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (level == "VOXDELAY") {
        // Hamlib seconds → raw 0-100 (1 unit = 20 ms)
        const int raw = qBound(0, qRound(val * 50.0), 100);
        QMetaObject::invokeMethod(m_model, [model = m_model, raw]() {
            model->transmitModel().setVoxDelay(raw);
        }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (level == "MONITOR_GAIN") {
        const int lvl = qBound(0, qRound(val * 100.0), 100);
        QMetaObject::invokeMethod(m_model, [model = m_model, lvl]() {
            model->transmitModel().setMonGainSb(lvl);
        }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (level == "BKINDL") {
        // Hamlib seconds → ms (0–2000 ms)
        const int ms = qBound(0, qRound(val * 1000.0), 2000);
        QMetaObject::invokeMethod(m_model, [model = m_model, ms]() {
            model->transmitModel().setCwDelay(ms);
        }, Qt::QueuedConnection);
        return rprt(0);
    }

    // Slice-dependent setters
    auto* slice = currentSlice();
    if (!slice) return rprt(-8);

    if (level == "AF") {
        const float gain = static_cast<float>(qBound(0.0, val * 100.0, 100.0));
        QMetaObject::invokeMethod(slice, [slice, gain]() {
            slice->setAudioGain(gain);
        }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (level == "RF") {
        const float gain = static_cast<float>(qBound(0.0, val * 100.0, 100.0));
        QMetaObject::invokeMethod(slice, [slice, gain]() {
            slice->setRfGain(gain);
        }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (level == "SQL") {
        const int lvl = qBound(0, qRound(val * 100.0), 100);
        const bool on = slice->squelchOn();
        QMetaObject::invokeMethod(slice, [slice, on, lvl]() {
            slice->setSquelch(on, lvl);
        }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (level == "APF") {
        const int lvl = qBound(0, qRound(val * 100.0), 100);
        QMetaObject::invokeMethod(slice, [slice, lvl]() {
            slice->setApfLevel(lvl);
        }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (level == "NR") {
        const int lvl = qBound(0, qRound(val * 100.0), 100);
        QMetaObject::invokeMethod(slice, [slice, lvl]() {
            slice->setNrLevel(lvl);
        }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (level == "NB") {
        const int lvl = qBound(0, qRound(val * 100.0), 100);
        QMetaObject::invokeMethod(slice, [slice, lvl]() {
            slice->setNbLevel(lvl);
        }, Qt::QueuedConnection);
        return rprt(0);
    }

    return rprt(-11);  // RIG_ENAVAIL
}

QString RigctlProtocol::cmdDumpState()
{
    // Mimic rigctld --model=2 (NET rigctl) output.
    // WSJT-X and fldigi parse this to discover rig capabilities.
    // Frequency range and mode set match FLEX-8600 fw v1.4.0.0 (30 kHz – 54 MHz).
    // Mode flags: AM=0x1, CW=0x2, USB=0x4, LSB=0x8, RTTY=0x10, FM=0x20,
    //             CWR=0x80, AMS=0x200, PKTLSB=0x400, PKTUSB=0x800
    // Combined: 0x4|0x8|0x2|0x80|0x1|0x20|0x10|0x200|0x800|0x400 = 0xEBF

    // Protocol v1 dump_state matching Hamlib 4.6.5 netrigctl_open().
    // Every field must be present or WSJT-X times out waiting for data.
    QString dump;
    dump += "1\n";                       // protocol version
    dump += "2\n";                       // rig model = NET rigctl
    dump += "0\n";                       // ITU region
    // RX range
    dump += "30000.000000 54000000.000000 0xebf -1 -1 0x40000003 0x3\n";
    dump += "0 0 0 0 0 0 0\n";          // end RX range
    // TX range
    dump += "30000.000000 54000000.000000 0xebf 1 100000 0x40000003 0x3\n";
    dump += "0 0 0 0 0 0 0\n";          // end TX range
    // Tuning steps
    dump += "0xebf 1\n";
    dump += "0xebf 10\n";
    dump += "0xebf 100\n";
    dump += "0xebf 1000\n";
    dump += "0 0\n";                     // end tuning steps
    // Filters
    dump += "0x2 500\n";
    dump += "0x2 200\n";
    dump += "0xc 2400\n";
    dump += "0xc 1800\n";
    dump += "0x1 6000\n";
    dump += "0x20 12000\n";
    dump += "0 0\n";                     // end filters
    // Max RIT/XIT/IF shift/announces
    dump += "9999\n";
    dump += "9999\n";
    dump += "0\n";
    dump += "0\n";
    // Preamp/attenuator (empty = none)
    dump += "\n";
    dump += "\n";
    // has get/set func/level/parm
    dump += QStringLiteral("0x%1\n").arg(kRigGetFuncMask, 0, 16);
    dump += QStringLiteral("0x%1\n").arg(kRigSetFuncMask, 0, 16);
    dump += QStringLiteral("0x%1\n").arg(kRigGetLevelMask, 0, 16);
    dump += QStringLiteral("0x%1\n").arg(kRigSetLevelMask, 0, 16);
    dump += "0x0\n";
    dump += "0x0\n";
    // Protocol v1 additional fields (required by netrigctl_open)
    dump += QStringLiteral("0x%1\n").arg(kRigVfoOpMask, 0, 16);  // vfo_ops (UP, DOWN)
    dump += "0\n";                       // ptt_type (RIG_PTT_NONE)
    dump += "0\n";                       // targetable_vfo
    dump += "1\n";                       // has_set_vfo
    dump += "1\n";                       // has_get_vfo
    dump += "1\n";                       // has_set_freq
    dump += "1\n";                       // has_get_freq
    dump += "0\n";                       // has_set_conf
    dump += "0\n";                       // has_get_conf
    dump += "1\n";                       // has_get_ant
    dump += "1\n";                       // has_set_ant
    dump += "1\n";                       // has_power2mW
    dump += "1\n";                       // has_mW2power
    dump += "0\n";                       // timeout (ms, 0 = default)
    dump += "done\n";                    // terminates the v1 extended fields

    // Clients using extended mode ('+' prefix) expect RPRT 0 as a terminator.
    // Bare dump_state (nc, WSJT-X) stops reading at "done\n" and never sees it.
    if (m_extended)
        dump += rprt(0);

    return dump;
}

QString RigctlProtocol::cmdSendMorse(const QString& text)
{
    if (!m_model) return rprt(-1);
    if (text.isEmpty()) {
        // Hamlib `b` accepts the morse text on the next line when none is
        // supplied inline. Arm the pending flag and return no response; the
        // next handleLine() call will deliver the text. Required by Not1MM
        // contest CW and any other client that uses the two-line form.
        m_pendingMorseLine = true;
        return {};
    }
    // Route through CwxModel so the local sidetone keyer (driven by
    // CwxModel::transmissionRequested) fires alongside the radio command.
    // Going through sendCmdPublic directly would silently bypass the
    // sidetone path used by the MIDI key and CWX panel. (#2909)
    QMetaObject::invokeMethod(m_model, [model = m_model, text]() {
        model->cwxModel().send(text);
    }, Qt::QueuedConnection);
    return rprt(0);
}

QString RigctlProtocol::cmdStopMorse()
{
    if (!m_model) return rprt(-1);
    // CwxModel::clearBuffer emits transmissionCancelled, which cuts any
    // in-flight local sidetone in addition to sending "cwx clear". (#2909)
    QMetaObject::invokeMethod(m_model, [model = m_model]() {
        model->cwxModel().clearBuffer();
    }, Qt::QueuedConnection);
    return rprt(0);
}

QString RigctlProtocol::cmdSetKeySpeed(const QString& arg)
{
    if (!m_model) return rprt(-1);
    bool ok = false;
    int wpm = arg.toInt(&ok);
    if (!ok || wpm < 5 || wpm > 100) return rprt(-1);
    QString cmd = QString("cw wpm %1").arg(wpm);
    QMetaObject::invokeMethod(m_model, [model = m_model, cmd]() {
        model->sendCmdPublic(cmd, nullptr);
    }, Qt::QueuedConnection);
    return rprt(0);
}

// ── RIT / XIT ───────────────────────────────────────────────────────────────

QString RigctlProtocol::cmdGetRit()
{
    auto* slice = currentSlice();
    if (!slice) return rprt(-8);
    const int hz = slice->ritOn() ? slice->ritFreq() : 0;
    if (m_extended)
        return QStringLiteral("get_rit:\nRIT: %1\n").arg(hz) + rprt(0);
    return QStringLiteral("%1\n").arg(hz);
}

QString RigctlProtocol::cmdSetRit(const QString& arg)
{
    auto* slice = currentSlice();
    if (!slice) return rprt(-8);
    bool ok;
    const int hz = arg.trimmed().toInt(&ok);
    if (!ok) return rprt(-1);
    QMetaObject::invokeMethod(slice, [slice, hz]() {
        slice->setRit(hz != 0, hz);
    }, Qt::QueuedConnection);
    return rprt(0);
}

QString RigctlProtocol::cmdGetXit()
{
    auto* slice = currentSlice();
    if (!slice) return rprt(-8);
    const int hz = slice->xitOn() ? slice->xitFreq() : 0;
    if (m_extended)
        return QStringLiteral("get_xit:\nXIT: %1\n").arg(hz) + rprt(0);
    return QStringLiteral("%1\n").arg(hz);
}

QString RigctlProtocol::cmdSetXit(const QString& arg)
{
    auto* slice = currentSlice();
    if (!slice) return rprt(-8);
    bool ok;
    const int hz = arg.trimmed().toInt(&ok);
    if (!ok) return rprt(-1);
    QMetaObject::invokeMethod(slice, [slice, hz]() {
        slice->setXit(hz != 0, hz);
    }, Qt::QueuedConnection);
    return rprt(0);
}

// ── get_func / set_func ─────────────────────────────────────────────────────

QString RigctlProtocol::cmdGetFunc(const QString& arg)
{
    const QString func = arg.trimmed().toUpper();

    if (func == "?") {
        const QString list = rigGetFuncTokens().join(' ');
        if (m_extended)
            return QStringLiteral("get_func:\nFuncs: %1\n").arg(list) + rprt(0);
        return list + "\n";
    }

    auto* slice = currentSlice();
    if (!slice) return rprt(-8);

    auto makeFuncResp = [this, &func](int val) {
        if (m_extended)
            return QStringLiteral("get_func: %1\n%2: %3\n").arg(func, func).arg(val) + rprt(0);
        return QStringLiteral("%1\n").arg(val);
    };

    const auto& txModel = m_model->transmitModel();

    if (func == "NB")    return makeFuncResp(slice->nbOn() ? 1 : 0);
    if (func == "NR")    return makeFuncResp(slice->nrOn() ? 1 : 0);
    if (func == "ANF")   return makeFuncResp(slice->anfOn() ? 1 : 0);
    if (func == "APF")   return makeFuncResp(slice->apfOn() ? 1 : 0);
    if (func == "LOCK")  return makeFuncResp(slice->isLocked() ? 1 : 0);
    if (func == "MUTE")  return makeFuncResp(slice->audioMute() ? 1 : 0);
    if (func == "SQL")   return makeFuncResp(slice->squelchOn() ? 1 : 0);
    if (func == "VOX")   return makeFuncResp(txModel.voxEnable() ? 1 : 0);
    if (func == "COMP")  return makeFuncResp(txModel.speechProcessorEnable() ? 1 : 0);
    if (func == "FBKIN") return makeFuncResp(txModel.cwBreakIn() ? 1 : 0);

    return rprt(-11);  // RIG_ENAVAIL
}

QString RigctlProtocol::cmdSetFunc(const QString& args)
{
    QStringList parts = args.split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return rprt(-1);

    const QString func = parts[0].toUpper();

    if (func == "?") {
        const QString list = rigGetFuncTokens().join(' ');
        if (m_extended)
            return QStringLiteral("set_func:\nFuncs: %1\n").arg(list) + rprt(0);
        return list + "\n";
    }

    if (parts.size() < 2) return rprt(-1);
    bool ok;
    const bool on = (parts[1].toInt(&ok) != 0);
    if (!ok) return rprt(-1);

    auto* slice = currentSlice();
    if (!slice) return rprt(-8);

    if (func == "NB") {
        QMetaObject::invokeMethod(slice, [slice, on]() { slice->setNb(on); }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (func == "NR") {
        QMetaObject::invokeMethod(slice, [slice, on]() { slice->setNr(on); }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (func == "ANF") {
        QMetaObject::invokeMethod(slice, [slice, on]() { slice->setAnf(on); }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (func == "APF") {
        QMetaObject::invokeMethod(slice, [slice, on]() { slice->setApf(on); }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (func == "LOCK") {
        QMetaObject::invokeMethod(slice, [slice, on]() { slice->setLocked(on); }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (func == "MUTE") {
        QMetaObject::invokeMethod(slice, [slice, on]() { slice->setAudioMute(on); }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (func == "SQL") {
        const int lvl = slice->squelchLevel();
        QMetaObject::invokeMethod(slice, [slice, on, lvl]() {
            slice->setSquelch(on, lvl);
        }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (func == "VOX") {
        QMetaObject::invokeMethod(m_model, [model = m_model, on]() {
            model->transmitModel().setVoxEnable(on);
        }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (func == "COMP") {
        QMetaObject::invokeMethod(m_model, [model = m_model, on]() {
            model->transmitModel().setSpeechProcessorEnable(on);
        }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (func == "FBKIN") {
        QMetaObject::invokeMethod(m_model, [model = m_model, on]() {
            model->transmitModel().setCwBreakIn(on);
        }, Qt::QueuedConnection);
        return rprt(0);
    }

    return rprt(-11);  // RIG_ENAVAIL
}

// ── get_ant / set_ant ───────────────────────────────────────────────────────

QString RigctlProtocol::cmdGetAnt()
{
    auto* slice = currentSlice();
    if (!slice) return rprt(-8);
    const int antMask = antNameToMask(slice->rxAntenna());
    if (m_extended)
        return QStringLiteral("get_ant:\nAnt: %1\nOption: 0\n").arg(antMask) + rprt(0);
    return QStringLiteral("%1\n0\n").arg(antMask);
}

QString RigctlProtocol::cmdSetAnt(const QString& arg)
{
    auto* slice = currentSlice();
    if (!slice) return rprt(-8);
    if (arg.trimmed() == "?") {
        // Build bitmask list from the slice's antenna list.
        QStringList masks;
        int bit = 1;
        for (int i = 0; i < slice->rxAntennaList().size(); ++i, bit <<= 1)
            masks << QString::number(bit);
        const QString list = masks.isEmpty() ? QStringLiteral("1") : masks.join(' ');
        if (m_extended)
            return QStringLiteral("set_ant:\nAnts: %1\n").arg(list) + rprt(0);
        return list + "\n";
    }
    bool ok;
    const int mask = arg.trimmed().toInt(&ok);
    if (!ok || mask < 0) return rprt(-1);
    const QString name = antMaskToName(mask, slice->rxAntennaList());
    QMetaObject::invokeMethod(slice, [slice, name]() {
        slice->setRxAntenna(name);
        slice->setTxAntenna(name);
    }, Qt::QueuedConnection);
    return rprt(0);
}

// ── get_ts / set_ts ─────────────────────────────────────────────────────────

QString RigctlProtocol::cmdGetTs()
{
    auto* slice = currentSlice();
    if (!slice) return rprt(-8);
    if (m_extended)
        return QStringLiteral("get_ts:\nTuning Step: %1\n").arg(slice->stepHz()) + rprt(0);
    return QStringLiteral("%1\n").arg(slice->stepHz());
}

QString RigctlProtocol::cmdSetTs(const QString& arg)
{
    auto* slice = currentSlice();
    if (!slice) return rprt(-8);
    if (arg.trimmed() == "?") {
        // Common tuning steps in Hz; 0 = any step accepted.
        static const QString kSteps = QStringLiteral("1 10 100 500 1000 5000 9000 10000 12500 100000 500000 0");
        if (m_extended)
            return QStringLiteral("set_ts:\nTuning Steps: %1\n").arg(kSteps) + rprt(0);
        return kSteps + "\n";
    }
    bool ok;
    const int hz = arg.trimmed().toInt(&ok);
    if (!ok || hz < 0) return rprt(-1);
    const int id = slice->sliceId();
    const QString cmd = QStringLiteral("slice set %1 step=%2").arg(id).arg(hz);
    QMetaObject::invokeMethod(m_model, [model = m_model, cmd]() {
        model->sendCmdPublic(cmd, nullptr);
    }, Qt::QueuedConnection);
    return rprt(0);
}

// ── get_dcd ─────────────────────────────────────────────────────────────────

QString RigctlProtocol::cmdGetDcd()
{
    auto* slice = currentSlice();
    if (!slice) return rprt(-8);
    // Without direct squelch-open status from the radio, report DCD open (1)
    // when squelch is disabled, and closed (0) when squelch is on.  This is
    // a conservative approximation — HF modes rarely use squelch, so DCD=1
    // is correct for most use cases.
    const int dcd = slice->squelchOn() ? 0 : 1;
    if (m_extended)
        return QStringLiteral("get_dcd:\nDCD: %1\n").arg(dcd) + rprt(0);
    return QStringLiteral("%1\n").arg(dcd);
}

// ── get_rig_info ────────────────────────────────────────────────────────────

QString RigctlProtocol::cmdGetRigInfo()
{
    QString info;
    if (!m_model || !m_model->isConnected()) {
        info = QStringLiteral("Info: AetherSDR (not connected)");
    } else {
        auto* slice = currentSlice();
        info = QStringLiteral("Info: %1 %2 v%3; Slice %4 %5 MHz %6")
            .arg(m_model->name(), m_model->model(), m_model->version())
            .arg(m_sliceIndex)
            .arg(slice ? slice->frequency() : 0.0, 0, 'f', 6)
            .arg(slice ? slice->mode() : QStringLiteral("-"));
    }
    if (m_extended)
        return info + "\n" + rprt(0);
    return info + "\n";
}

// ── power2mW / mW2power ─────────────────────────────────────────────────────

// ── get_trn / set_trn ───────────────────────────────────────────────────────

QString RigctlProtocol::cmdGetTrn()
{
    // Transceive mode: we don't push async status changes to CAT clients.
    // Report 0 (RIG_TRN_OFF) so MacLoggerDX and fldigi don't attempt to
    // enable a notification channel we can't service.
    if (m_extended)
        return QStringLiteral("get_trn:\nTransceive: 0\n") + rprt(0);
    return QStringLiteral("0\n");
}

QString RigctlProtocol::cmdSetTrn(const QString& arg)
{
    // Accept any transceive mode request silently.  Returning RPRT 0 prevents
    // MacLoggerDX from treating the absence of async callbacks as a fatal error.
    Q_UNUSED(arg)
    return rprt(0);
}

// ── vfo_op ──────────────────────────────────────────────────────────────────

QString RigctlProtocol::cmdVfoOp(const QString& arg)
{
    const QString op = arg.trimmed().toUpper();

    if (op == "?") {
        // Return supported VFO operation bitmask (UP | DOWN)
        if (m_extended)
            return QStringLiteral("vfo_op:\nVFO Op: 0x%1\n").arg(kRigVfoOpMask, 0, 16) + rprt(0);
        return QStringLiteral("0x%1\n").arg(kRigVfoOpMask, 0, 16);
    }

    auto* slice = currentSlice();
    if (!slice) return rprt(-8);

    if (op == "UP") {
        const double mhz = slice->frequency() + slice->stepHz() / 1e6;
        QMetaObject::invokeMethod(slice, [slice, mhz]() {
            slice->setFrequency(mhz);
        }, Qt::QueuedConnection);
        return rprt(0);
    }
    if (op == "DOWN") {
        const double mhz = slice->frequency() - slice->stepHz() / 1e6;
        QMetaObject::invokeMethod(slice, [slice, mhz]() {
            slice->setFrequency(mhz);
        }, Qt::QueuedConnection);
        return rprt(0);
    }

    return rprt(-11);  // RIG_ENAVAIL
}

// ── get_vfo_info ────────────────────────────────────────────────────────────

QString RigctlProtocol::cmdGetVfoInfo(const QString& arg)
{
    // Hamlib 4.1+ command: returns freq, mode, passband, and split state for a
    // named VFO in one round trip.  MacLoggerDX queries both VFOA and VFOB on
    // connect to build its initial rig state snapshot.
    const QString vfo = arg.trimmed().toUpper();

    // Resolve which slice the requested VFO label refers to.
    SliceModel* slice = nullptr;
    if (vfo == "VFOB" || vfo == "SUB") {
        slice = findTxSlice();
        if (!slice) slice = currentSlice();  // no split: VFOB == VFOA
    } else {
        slice = currentSlice();  // VFOA, MAIN, or unrecognised → current
    }
    if (!slice) return rprt(-8);

    const long long hz      = static_cast<long long>(std::round(slice->frequency() * 1e6));
    const QString hamlibMode = smartsdrToHamlib(slice->mode());
    const int passband      = qAbs(slice->filterHigh() - slice->filterLow());

    auto* rxSlice = currentSlice();
    auto* txSlice = findTxSlice();
    const bool split = (rxSlice && txSlice && rxSlice != txSlice);

    if (m_extended) {
        // Echo line must include the VFO argument ("get_vfo_info: VFOA").
        // Clients split on ": " to extract the command key; without the
        // argument the key becomes "get_vfo_info:" which fails the lookup.
        return QStringLiteral("get_vfo_info: %1\nFreq: %2\nMode: %3\nWidth: %4\nSplit: %5\nSatMode: 0\n")
                   .arg(vfo).arg(hz).arg(hamlibMode).arg(passband).arg(split ? 1 : 0)
               + rprt(0);
    }
    return QStringLiteral("%1\n%2\n%3\n%4\n0\n")
               .arg(hz).arg(hamlibMode).arg(passband).arg(split ? 1 : 0);
}

QString RigctlProtocol::cmdPower2mW(const QString& args)
{
    // Args format: "freq ratio mode" — parts[0]=freq, parts[1]=ratio (0.0–1.0)
    QStringList parts = args.split(' ', Qt::SkipEmptyParts);
    if (parts.size() < 2) return rprt(-1);
    bool ok;
    const double ratio = parts[1].toDouble(&ok);
    if (!ok) return rprt(-1);
    const int maxW = m_model ? qMax(1, m_model->transmitModel().maxPowerLevel()) : 100;
    const long long mW = qRound64(qBound(0.0, ratio, 1.0) * maxW * 1000.0);
    if (m_extended)
        return QStringLiteral("power2mW:\nPower mW: %1\n").arg(mW) + rprt(0);
    return QStringLiteral("%1\n").arg(mW);
}

QString RigctlProtocol::cmdMW2power(const QString& args)
{
    // Args format: "freq mW mode" — parts[0]=freq, parts[1]=mW
    QStringList parts = args.split(' ', Qt::SkipEmptyParts);
    if (parts.size() < 2) return rprt(-1);
    bool ok;
    const double mW = parts[1].toDouble(&ok);
    if (!ok) return rprt(-1);
    const int maxW = m_model ? qMax(1, m_model->transmitModel().maxPowerLevel()) : 100;
    const double ratio = qBound(0.0, mW / (maxW * 1000.0), 1.0);
    if (m_extended)
        return QStringLiteral("mW2power:\nPower: %1\n").arg(formatRigLevelValue(ratio)) + rprt(0);
    return QStringLiteral("%1\n").arg(formatRigLevelValue(ratio));
}

} // namespace AetherSDR
