#include "RigctlProtocol.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QLocale>
#include <QMetaObject>
#include <QStringList>
#include <QtGlobal>

namespace AetherSDR {

namespace {

constexpr quint64 kRigLevelRfPower           = (1ULL << 13);
constexpr quint64 kRigLevelKeyspd            = (1ULL << 14);
constexpr quint64 kRigLevelSwr               = (1ULL << 28);
constexpr quint64 kRigLevelRfPowerMeter      = (1ULL << 32);
constexpr quint64 kRigLevelRfPowerMeterWatts = (1ULL << 39);
constexpr qint64  kTxMeterFreshMs            = 1500;
constexpr quint64 kRigGetLevelMask = kRigLevelRfPower
                                   | kRigLevelKeyspd
                                   | kRigLevelSwr
                                   | kRigLevelRfPowerMeter
                                   | kRigLevelRfPowerMeterWatts;
constexpr quint64 kRigSetLevelMask = kRigLevelRfPower
                                   | kRigLevelKeyspd;

QStringList rigGetLevelTokens()
{
    return {
        QStringLiteral("RFPOWER"),
        QStringLiteral("KEYSPD"),
        QStringLiteral("SWR"),
        QStringLiteral("RFPOWER_METER"),
        QStringLiteral("RFPOWER_METER_WATTS"),
    };
}

QStringList rigSetLevelTokens()
{
    return {
        QStringLiteral("RFPOWER"),
        QStringLiteral("KEYSPD"),
    };
}

QString formatRigLevelValue(double value)
{
    QString text = QLocale::c().toString(value, 'g', 6);
    if (text == "-0" || text == "-0.0")
        text = "0";
    return text;
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
        if (name == "get_split_vfo")  return cmdGetSplitVfo();
        if (name == "set_split_vfo")  return cmdSetSplitVfo(args);
        if (name == "get_split_freq") return cmdGetSplitFreq();
        if (name == "set_split_freq") return cmdSetSplitFreq(args);
        if (name == "get_split_mode") return cmdGetSplitMode();
        if (name == "set_split_mode") return cmdSetSplitMode(args);
        if (name == "get_level")      return cmdGetLevel(args);
        if (name == "set_level")      return cmdSetLevel(args);
        if (name == "dump_state")     return cmdDumpState();
        if (name == "quit")           return {};  // caller handles disconnect
        if (name == "chk_vfo")        return QStringLiteral("0\n");  // VFO mode disabled
        if (name == "send_morse")    return cmdSendMorse(args);
        if (name == "stop_morse")    return cmdStopMorse();

        return rprt(-4);  // RIG_EINVAL
    }

    // Short form: single character + optional args
    QChar shortCmd = cmd[0];
    QString args = cmd.mid(1).trimmed();

    switch (shortCmd.toLatin1()) {
    case 'f': return cmdGetFreq();
    case 'F': return cmdSetFreq(args);
    case 'm': return cmdGetMode();
    case 'M': return cmdSetMode(args);
    case 'v': return cmdGetVfo();
    case 'V': return cmdSetVfo(args);
    case 't': return cmdGetPtt();
    case 'T': return cmdSetPtt(args);
    case '_': return cmdGetInfo();
    case 's': return cmdGetSplitVfo();
    case 'S': return cmdSetSplitVfo(args);
    case 'i': return cmdGetSplitFreq();
    case 'I': return cmdSetSplitFreq(args);
    case 'x': return cmdGetSplitMode();
    case 'X': return cmdSetSplitMode(args);
    case 'l': return cmdGetLevel(args);
    case 'L': return cmdSetLevel(args);
    case '1': return cmdDumpState();       // \dump_state
    case 'b': return cmdSendMorse(args);    // send morse
    case 'q': return {};                   // quit
    default:  return rprt(-4);             // RIG_EINVAL
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
    QMetaObject::invokeMethod(m_model, [this, tx]() {
        // When keying TX, move the TX badge to this protocol's bound slice
        // so the correct slice is used for transmission.
        if (tx) {
            auto* slice = currentSlice();
            if (slice && !slice->isTxSlice())
                slice->setTxSlice(true);
        }
        m_model->setTransmit(tx, TransmitModel::PttSource::Dax);
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
SliceModel* RigctlProtocol::findTxSlice() const
{
    if (!m_model) return nullptr;
    for (auto* s : m_model->slices())
        if (s->isTxSlice()) return s;
    return nullptr;
}

QString RigctlProtocol::cmdGetSplitVfo()
{
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
    bool enable = (parts[0] == "1");
    if (!enable) {
        // Disable split — move TX back to our slice (idempotent).
        // Hamlib clients (VARA/VARAC/N1MM) poll this every ~3s; without the
        // isTxSlice() guard the radio gets bombarded with `slice set N tx=1`
        // commands, which appears to interfere with radio-side TX state
        // tracking and causes spurious unkey after ~1s during PTT.
        if (auto* s = currentSlice(); s && !s->isTxSlice())
            s->setTxSlice(true);
    }
    // Enabling split requires a second slice — handled by MainWindow's split logic
    return rprt(0);
}

QString RigctlProtocol::cmdGetSplitFreq()
{
    auto* txSlice = findTxSlice();
    if (!txSlice) return rprt(-1);
    long long hz = static_cast<long long>(std::round(txSlice->frequency() * 1e6));
    if (m_extended)
        return QString("get_split_freq:\nTX Frequency: %1\n").arg(hz) + rprt(0);
    return QString("%1\n").arg(hz);
}

QString RigctlProtocol::cmdSetSplitFreq(const QString& args)
{
    auto* txSlice = findTxSlice();
    if (!txSlice) return rprt(-1);
    bool ok;
    double hz = args.trimmed().toDouble(&ok);
    if (!ok) return rprt(-1);
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
    auto* txSlice = findTxSlice();
    if (!txSlice) return rprt(-1);
    QStringList parts = args.split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return rprt(-1);
    QString mode = parts[0];
    if (mode == "PKTUSB") mode = "DIGU";
    else if (mode == "PKTLSB") mode = "DIGL";
    else if (mode == "AMS") mode = "SAM";
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
            return QStringLiteral("get_level:\nLevel: %1\nLevel Value: %2\n")
                .arg(level, value) + rprt(0);
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
        QMetaObject::invokeMethod(m_model, [this, percent]() {
            m_model->transmitModel().setRfPower(percent);
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
    // get levels: RFPOWER, KEYSPD, SWR, RFPOWER_METER, RFPOWER_METER_WATTS
    // set levels: RFPOWER, KEYSPD
    dump += "0x0\n";
    dump += "0x0\n";
    dump += QStringLiteral("0x%1\n").arg(kRigGetLevelMask, 0, 16);
    dump += QStringLiteral("0x%1\n").arg(kRigSetLevelMask, 0, 16);
    dump += "0x0\n";
    dump += "0x0\n";
    // Protocol v1 additional fields (required by netrigctl_open)
    dump += "0\n";                       // vfo_ops
    dump += "0\n";                       // ptt_type (RIG_PTT_NONE)
    dump += "0\n";                       // targetable_vfo
    dump += "1\n";                       // has_set_vfo
    dump += "1\n";                       // has_get_vfo
    dump += "1\n";                       // has_set_freq
    dump += "1\n";                       // has_get_freq
    dump += "0\n";                       // has_set_conf
    dump += "0\n";                       // has_get_conf
    dump += "0\n";                       // has_get_ant (no antenna control via rigctld)
    dump += "0\n";                       // has_set_ant
    dump += "0\n";                       // has_power2mW
    dump += "0\n";                       // has_mW2power
    dump += "0\n";                       // timeout (ms, 0 = default)
    dump += "done\n";                    // terminates the v1 extended fields

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
    QString cmd = QString("cwx send \"%1\"").arg(text);
    QMetaObject::invokeMethod(m_model, [this, cmd]() {
        m_model->sendCmdPublic(cmd, nullptr);
    }, Qt::QueuedConnection);
    return rprt(0);
}

QString RigctlProtocol::cmdStopMorse()
{
    if (!m_model) return rprt(-1);
    QMetaObject::invokeMethod(m_model, [this]() {
        m_model->sendCmdPublic("cwx clear", nullptr);
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
    QMetaObject::invokeMethod(m_model, [this, cmd]() {
        m_model->sendCmdPublic(cmd, nullptr);
    }, Qt::QueuedConnection);
    return rprt(0);
}

} // namespace AetherSDR
