#include "SmartCatProtocol.h"
#include "LogManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"
#include "models/CwxModel.h"

#include <algorithm>

namespace AetherSDR {

// ── Mode conversion tables ──────────────────────────────────────────────────
//
// SmartSDR mode strings verified against FLEX-8600 fw v1.4.0.0.
// Kenwood 1-digit codes: 1=LSB 2=USB 3=CW 4=FM 5=AM 6=DIGL 9=DIGU
// ZZ 2-digit codes from SmartSDR CAT User Guide Rev. v4.1.5:
//   00=LSB 01=USB 03=CWL 04=CWU(CW) 05=FM 06=AM 07=DIGU 09=DIGL
//   10=SAM 11=NFM 12=DFM 20=FDV 30=RTTY 40=DSTR

QString SmartCatProtocol::modeToKenwood(const QString& ssdrMode)
{
    if (ssdrMode == "LSB")  return "1";
    if (ssdrMode == "USB")  return "2";
    if (ssdrMode == "CW")   return "3";
    if (ssdrMode == "CWL")  return "3";
    if (ssdrMode == "FM")   return "4";
    if (ssdrMode == "NFM")  return "4";
    if (ssdrMode == "AM")   return "5";
    if (ssdrMode == "SAM")  return "5";
    if (ssdrMode == "DIGL") return "6";
    if (ssdrMode == "RTTY") return "6";
    if (ssdrMode == "DIGU") return "9";
    if (ssdrMode == "FDV")  return "9";
    return "2";
}

QString SmartCatProtocol::modeToZZ(const QString& ssdrMode)
{
    if (ssdrMode == "LSB")  return "00";
    if (ssdrMode == "USB")  return "01";
    if (ssdrMode == "CWL")  return "03";
    if (ssdrMode == "CW")   return "04";
    if (ssdrMode == "FM")   return "05";
    if (ssdrMode == "AM")   return "06";
    if (ssdrMode == "DIGU") return "07";
    if (ssdrMode == "DIGL") return "09";
    if (ssdrMode == "SAM")  return "10";
    if (ssdrMode == "NFM")  return "11";
    if (ssdrMode == "DFM")  return "12";
    if (ssdrMode == "FDV")  return "20";
    if (ssdrMode == "RTTY") return "30";
    if (ssdrMode == "DSTR") return "40";
    return "01";
}

QString SmartCatProtocol::kenwoodToSSDR(QChar c)
{
    switch (c.toLatin1()) {
        case '1': return "LSB";
        case '2': return "USB";
        case '3': return "CW";
        case '4': return "FM";
        case '5': return "AM";
        case '6': return "DIGL";
        case '9': return "DIGU";
        default:  return "USB";
    }
}

QString SmartCatProtocol::zzToSSDR(const QString& two)
{
    if (two == "00") return "LSB";
    if (two == "01") return "USB";
    if (two == "03") return "CWL";
    if (two == "04") return "CW";
    if (two == "05") return "FM";
    if (two == "06") return "AM";
    if (two == "07") return "DIGU";
    if (two == "09") return "DIGL";
    if (two == "10") return "SAM";
    if (two == "11") return "NFM";
    if (two == "12") return "DFM";
    if (two == "20") return "FDV";
    if (two == "30") return "RTTY";
    if (two == "40") return "DSTR";
    return "USB";
}

QString SmartCatProtocol::freqField(double mhz)
{
    quint64 hz = static_cast<quint64>(mhz * 1e6 + 0.5);
    return QString::number(hz).rightJustified(11, '0');
}

// ── Constructor ─────────────────────────────────────────────────────────────

SmartCatProtocol::SmartCatProtocol(RadioModel* model, int vfoA, int vfoB,
                                   bool flexExtensions)
    : m_model(model)
    , m_vfoA(vfoA)
    , m_vfoB(vfoB)
    , m_flexExtensions(flexExtensions)
{}

// ── Slice accessors ─────────────────────────────────────────────────────────

SliceModel* SmartCatProtocol::sliceA() const
{
    if (!m_model) return nullptr;
    for (auto* s : m_model->slices()) {
        if (s->sliceId() == m_vfoA) return s;
    }
    const auto& slices = m_model->slices();
    return slices.isEmpty() ? nullptr : slices.first();
}

SliceModel* SmartCatProtocol::sliceB() const
{
    if (m_vfoB < 0 || !m_model) return nullptr;
    for (auto* s : m_model->slices()) {
        if (s->sliceId() == m_vfoB) return s;
    }
    return nullptr;
}

// ── Command dispatcher ──────────────────────────────────────────────────────

QString SmartCatProtocol::processCommand(const QString& cmd)
{
    if (!cmd.isEmpty())
        qCDebug(lcCat).noquote() << "CAT ←" << cmd;
    const QString resp = processCommandImpl(cmd);
    if (!resp.isEmpty())
        qCDebug(lcCat).noquote() << "CAT →" << resp.trimmed();
    return resp;
}

QString SmartCatProtocol::processCommandImpl(const QString& cmd)
{
    if (cmd.isEmpty()) return {};

    const QString upper = cmd.toUpper();

    if (m_flexExtensions && upper.startsWith("ZZ") && upper.size() >= 4) {
        const QString name = upper.left(4);
        const QString arg  = cmd.mid(4);
        if (name == "ZZFA") {
            // cmdFA returns "FA...;"; prepend "ZZ" so the response matches the command prefix.
            // Set commands and errors (?;) pass through unchanged.
            QString r = cmdFA(arg);
            return r.startsWith(QLatin1String("FA")) ? QStringLiteral("ZZ") + r : r;
        }
        if (name == "ZZFB") {
            QString r = cmdFB(arg);
            return r.startsWith(QLatin1String("FB")) ? QStringLiteral("ZZ") + r : r;
        }
        if (name == "ZZMD") return cmdZZMD(arg);
        if (name == "ZZME") return cmdZZME(arg);
        if (name == "ZZIF") return cmdZZIF();
        if (name == "ZZAI") {
            // AI query — the session handles enable/disable; we just report state
            if (arg.isEmpty() || arg == "?")
                return QString("ZZAI%1;").arg(m_aiEnabled ? "01" : "00");
            return {};  // set handled by session
        }
        if (name == "ZZSW") return cmdZZSW(arg);
        if (name == "ZZTX") return cmdTX(arg);
        if (name == "ZZRX") return cmdRX();
        if (name == "ZZSM") return cmdZZSM(arg);
        // ── Tier-2 ZZ commands ──────────────────────────────────────────────
        if (name == "ZZAG") return cmdZZAG(arg);
        if (name == "ZZAR") return cmdZZAR(arg);
        if (name == "ZZAS") return cmdZZAS(arg);
        if (name == "ZZBI") return cmdZZBI(arg);
        if (name == "ZZDE") return cmdZZDE(arg);
        if (name == "ZZFI") return cmdZZFI(arg);
        if (name == "ZZFJ") return cmdZZFJ(arg);
        if (name == "ZZFR") return cmdZZFR();
        if (name == "ZZFT") { m_splitEnabled = !m_splitEnabled; return {}; }
        if (name == "ZZGT") return cmdZZGT(arg);
        if (name == "ZZLE") return cmdZZLE(arg);
        if (name == "ZZLB") return cmdZZLB(arg);
        if (name == "ZZLF") return cmdZZLF(arg);
        if (name == "ZZMA") return cmdZZMA(arg);
        if (name == "ZZMB") return cmdZZMB(arg);
        if (name == "ZZMG") return cmdZZMG(arg);
        if (name == "ZZNL") return cmdZZNL(arg);
        if (name == "ZZNR") return cmdZZNR(arg);
        if (name == "ZZPC") return cmdZZPC(arg);
        if (name == "ZZRC") return cmdZZRC();
        if (name == "ZZRD") return cmdZZRD(arg);
        if (name == "ZZRG") return cmdZZRG(arg);
        if (name == "ZZRT") return cmdZZRT(arg);
        if (name == "ZZRU") return cmdZZRU(arg);
        if (name == "ZZRW") return cmdZZRW(arg);
        if (name == "ZZRY") return cmdZZRY(arg);
        if (name == "ZZXC") return cmdZZXC();
        if (name == "ZZXG") return cmdZZXG(arg);
        if (name == "ZZXS") return cmdZZXS(arg);
        return "?;";
    }

    if (upper.size() >= 2) {
        const QString name = upper.left(2);
        const QString arg  = cmd.mid(2);
        if (name == "BY") return cmdBY(arg);
        if (name == "DN") return cmdDN(arg);
        if (name == "FA") return cmdFA(arg);
        if (name == "FB") return cmdFB(arg);
        if (name == "FW") return cmdFW(arg);
        if (name == "MD") return cmdMD(arg);
        if (name == "IF") return cmdIF();
        if (name == "AI") {
            // AI query — report state; session handles wiring
            if (arg.isEmpty())
                return QString("AI%1;").arg(m_aiEnabled ? 1 : 0);
            return {};  // set handled by session
        }
        if (name == "FT") return cmdFT(arg);
        if (name == "FR") return cmdFR(arg);
        if (name == "LK") return cmdLK(arg);
        if (name == "MG") return cmdMG(arg);
        if (name == "NB") return cmdNB(arg);
        if (name == "NL") return cmdNL(arg);
        if (name == "NR") return cmdNR(arg);
        if (name == "NT") return cmdNT(arg);
        if (name == "OI") return cmdOI();
        if (name == "PA") return cmdPA(arg);
        if (name == "TX") return cmdTX(arg);
        if (name == "RX") return cmdRX();
        if (name == "ID") return cmdID();
        if (name == "PS") return cmdPS();
        if (name == "RA") return cmdRA(arg);
        if (name == "RL") return cmdRL(arg);
        if (name == "RM") return cmdRM(arg);
        if (name == "SM") return cmdSM(arg);
        if (name == "SQ") return cmdSQ(arg);
        if (name == "TY") return cmdTY(arg);
        if (name == "UP") return cmdUP(arg);
        // ── Tier-2 base commands ─────────────────────────────────────────────
        if (name == "AG") return cmdAG(arg);
        if (name == "GT") return cmdGT(arg);
        if (name == "KS") return cmdKS(arg);
        if (name == "KY") return cmdKY(arg);
        if (name == "PC") return cmdPC(arg);
        if (name == "PT") return cmdPT(arg);
        if (name == "RC") return cmdRC();
        if (name == "RD") return cmdRD(arg);
        if (name == "RG") return cmdRG(arg);
        if (name == "RT") return cmdRT(arg);
        if (name == "RU") return cmdRU(arg);
        if (name == "SL") return cmdSL(arg);
        if (name == "SH") return cmdSH(arg);
        if (name == "VX") {
            // VOX enable/disable — stub; report VOX off, accept set silently.
            if (arg.isEmpty()) return QStringLiteral("VX0;");
            return {};
        }
        if (name == "XT") return cmdXT(arg);
        return "?;";
    }

    return "?;";
}

// ── FA / ZZFA — VFO A frequency ─────────────────────────────────────────────

QString SmartCatProtocol::cmdFA(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return "FA" + freqField(a->frequency()) + ";";
    bool ok;
    double hz = arg.toDouble(&ok);
    if (!ok) return "?;";
    a->setFrequency(hz / 1e6);
    return {};
}

// ── FB / ZZFB — VFO B frequency ─────────────────────────────────────────────

QString SmartCatProtocol::cmdFB(const QString& arg)
{
    SliceModel* b = sliceB();
    if (!b) {
        SliceModel* a = sliceA();
        if (!a) return "?;";
        if (arg.isEmpty())
            return "FB" + freqField(a->frequency()) + ";";
        return {};
    }
    if (arg.isEmpty())
        return "FB" + freqField(b->frequency()) + ";";
    bool ok;
    double hz = arg.toDouble(&ok);
    if (!ok) return "?;";
    b->setFrequency(hz / 1e6);
    return {};
}

// ── MD — mode get/set (Kenwood 1-digit) ──────────────────────────────────────

QString SmartCatProtocol::cmdMD(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return "MD" + modeToKenwood(a->mode()) + ";";
    if (arg.size() != 1) return "?;";
    a->setMode(kenwoodToSSDR(arg[0]));
    return {};
}

// ── ZZMD — mode get/set (ZZ 2-digit) ─────────────────────────────────────────

QString SmartCatProtocol::cmdZZMD(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return "ZZMD" + modeToZZ(a->mode()) + ";";
    if (arg.size() != 2) return "?;";
    a->setMode(zzToSSDR(arg));
    return {};
}

// ── ZZME — VFO B mode get/set ────────────────────────────────────────────────

QString SmartCatProtocol::cmdZZME(const QString& arg)
{
    SliceModel* b = sliceB();
    if (!b) return "?;";
    if (arg.isEmpty())
        return "ZZME" + modeToZZ(b->mode()) + ";";
    if (arg.size() != 2) return "?;";
    b->setMode(zzToSSDR(arg));
    return {};
}

// ── IF — composite Kenwood TS-2000 status (35-char body) ─────────────────────
//
// P1[0-10]  11-digit VFO-A Hz    P2[11-15] step 5 digits
// P3[16-21] RIT+sign+5            P4[22] RIT  P5[23] XIT
// P6[24-25] memory                P7[26] TX/RX  P8[27] mode
// P9[28] function  P10[29] scan   P11[30] split
// P12[31] tone  P13[32-33] CTCSS  P14[34] DCS

QString SmartCatProtocol::cmdIF()
{
    SliceModel* a = sliceA();
    if (!a) return "?;";

    QString body;
    body += freqField(a->frequency());
    body += QStringLiteral("00010");
    body += QStringLiteral("+00000");
    body += '0';
    body += '0';
    body += QStringLiteral("00");
    body += m_model->isRadioTransmitting() ? '1' : '0';
    body += modeToKenwood(a->mode());
    body += '0';
    body += '0';
    body += m_splitEnabled ? '1' : '0';
    body += '0';
    body += QStringLiteral("00");
    body += '0';

    return "IF" + body + ";";
}

// ── ZZIF — SmartSDR extended IF ───────────────────────────────────────────────

QString SmartCatProtocol::cmdZZIF()
{
    QString resp = cmdIF();
    if (resp == "?;") return "?;";
    return "ZZ" + resp;
}

// ── FT / ZZSW — split enable ─────────────────────────────────────────────────

QString SmartCatProtocol::cmdFT(const QString& arg)
{
    if (arg.isEmpty())
        return QString("FT%1;").arg(m_splitEnabled ? 1 : 0);
    m_splitEnabled = (arg == "1");
    return {};
}

QString SmartCatProtocol::cmdZZSW(const QString& arg)
{
    if (arg.isEmpty())
        return QString("ZZSW%1;").arg(m_splitEnabled ? "1" : "0");
    m_splitEnabled = (arg == "1");
    return {};
}

// ── FR — RX VFO select (accepted, no-op) ─────────────────────────────────────

QString SmartCatProtocol::cmdFR(const QString& /*arg*/)
{
    return {};
}

// ── TX / ZZTX — PTT on ───────────────────────────────────────────────────────
// P1: 0 = PTT off (alias for RX;), 1/2 = PTT on selecting mic source.
// Use PttSource::Dax to match rigctld: CAT callers must bypass local voice-mode
// interlocks so the radio itself is authoritative.

QString SmartCatProtocol::cmdTX(const QString& arg)
{
    if (arg == "0") return cmdRX();
    m_pttAssertedByMe = true;
    m_model->setTransmit(true, TransmitModel::PttSource::Dax);
    return {};
}

// ── RX / ZZRX — PTT off ──────────────────────────────────────────────────────

QString SmartCatProtocol::cmdRX()
{
    m_pttAssertedByMe = false;
    m_model->setTransmit(false, TransmitModel::PttSource::Dax);
    return {};
}

// ── PTT safety release ───────────────────────────────────────────────────────

void SmartCatProtocol::releasePtt()
{
    if (!m_pttAssertedByMe) return;
    m_pttAssertedByMe = false;
    m_model->setTransmit(false, TransmitModel::PttSource::Dax);
}

// ── ID — rig identification ───────────────────────────────────────────────────
// Table from SmartSDR CAT User Guide Rev. v4.1.5:
//   904=FLEX-6700  905=FLEX-6500  906=FLEX-6700R  907=FLEX-6300
//   908=FLEX-6400/M  909=FLEX-6600/M  910=FLEX-8400/M  911=FLEX-8600/M
//   930=AU-510/M  931=AU-520/M

QString SmartCatProtocol::cmdID()
{
    // TS-2000 dialect: identify as Kenwood TS-2000 (ID019 per Kenwood CAT spec)
    if (!m_flexExtensions)
        return QStringLiteral("ID019;");

    const QString m = m_model->model();
    if (m.contains("6700R")) return QStringLiteral("ID906;");
    if (m.contains("6700"))  return QStringLiteral("ID904;");
    if (m.contains("6500"))  return QStringLiteral("ID905;");
    if (m.contains("6300"))  return QStringLiteral("ID907;");
    if (m.contains("6400"))  return QStringLiteral("ID908;");
    if (m.contains("6600"))  return QStringLiteral("ID909;");
    if (m.contains("8400"))  return QStringLiteral("ID910;");
    if (m.contains("8600"))  return QStringLiteral("ID911;");
    if (m.contains("AU-510")) return QStringLiteral("ID930;");
    if (m.contains("AU-520")) return QStringLiteral("ID931;");
    return QStringLiteral("ID919;");
}

// ── PS — power status ────────────────────────────────────────────────────────

QString SmartCatProtocol::cmdPS()
{
    return QStringLiteral("PS1;");
}

// ── SM / ZZSM — S-meter (Tier 1: always 0) ──────────────────────────────────

QString SmartCatProtocol::cmdSM(const QString& /*arg*/)
{
    return QStringLiteral("SM00000;");
}

QString SmartCatProtocol::cmdZZSM(const QString& /*arg*/)
{
    return QStringLiteral("ZZSM0000;");
}

// ── Tier-2 helpers (file-scope) ──────────────────────────────────────────────

static QString fmt3(int v)
{
    return QString::number(v).rightJustified(3, '0');
}

static QString ritField(int hz)
{
    return (hz >= 0 ? QStringLiteral("+") : QStringLiteral("-"))
           + QString::number(qAbs(hz)).rightJustified(5, '0');
}

static int agcModeToZZ(const QString& mode)
{
    if (mode == "off")  return 0;
    if (mode == "slow") return 2;
    if (mode == "med")  return 3;
    if (mode == "fast") return 4;
    return 3;
}

static QString zzToAgcMode(const QString& code)
{
    if (code == "0") return "off";
    if (code == "2") return "slow";
    if (code == "3") return "med";
    if (code == "4") return "fast";
    return "med";
}

static constexpr int kRitMaxHz = 9999;

// ── AG / ZZAG — VFO A audio gain (0-100, 3-digit) ───────────────────────────

QString SmartCatProtocol::cmdAG(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return "AG" + fmt3(static_cast<int>(a->audioGain() + 0.5f)) + ";";
    bool ok;
    int v = arg.toInt(&ok);
    if (!ok || v < 0 || v > 100) return "?;";
    a->setAudioGain(static_cast<float>(v));
    return {};
}

QString SmartCatProtocol::cmdZZAG(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return "ZZAG" + fmt3(static_cast<int>(a->audioGain() + 0.5f)) + ";";
    bool ok;
    int v = arg.toInt(&ok);
    if (!ok || v < 0 || v > 100) return "?;";
    a->setAudioGain(static_cast<float>(v));
    return {};
}

// ── ZZLE — VFO B audio gain (0-100, 3-digit) ────────────────────────────────

QString SmartCatProtocol::cmdZZLE(const QString& arg)
{
    SliceModel* b = sliceB();
    if (!b) return "?;";
    if (arg.isEmpty())
        return "ZZLE" + fmt3(static_cast<int>(b->audioGain() + 0.5f)) + ";";
    bool ok;
    int v = arg.toInt(&ok);
    if (!ok || v < 0 || v > 100) return "?;";
    b->setAudioGain(static_cast<float>(v));
    return {};
}

// ── ZZLB — VFO A audio pan (0-100, 3-digit; 0=full left, 50=center) ─────────

QString SmartCatProtocol::cmdZZLB(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return "ZZLB" + fmt3(a->audioPan()) + ";";
    bool ok;
    int v = arg.toInt(&ok);
    if (!ok || v < 0 || v > 100) return "?;";
    a->setAudioPan(v);
    return {};
}

// ── ZZLF — VFO B audio pan ───────────────────────────────────────────────────

QString SmartCatProtocol::cmdZZLF(const QString& arg)
{
    SliceModel* b = sliceB();
    if (!b) return "?;";
    if (arg.isEmpty())
        return "ZZLF" + fmt3(b->audioPan()) + ";";
    bool ok;
    int v = arg.toInt(&ok);
    if (!ok || v < 0 || v > 100) return "?;";
    b->setAudioPan(v);
    return {};
}

// ── ZZMA — VFO A mute (0/1) ─────────────────────────────────────────────────

QString SmartCatProtocol::cmdZZMA(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return QString("ZZMA%1;").arg(a->audioMute() ? 1 : 0);
    if (arg != "0" && arg != "1") return "?;";
    a->setAudioMute(arg == "1");
    return {};
}

// ── ZZMB — VFO B mute (0/1) ─────────────────────────────────────────────────

QString SmartCatProtocol::cmdZZMB(const QString& arg)
{
    SliceModel* b = sliceB();
    if (!b) return "?;";
    if (arg.isEmpty())
        return QString("ZZMB%1;").arg(b->audioMute() ? 1 : 0);
    if (arg != "0" && arg != "1") return "?;";
    b->setAudioMute(arg == "1");
    return {};
}

// ── GT / ZZGT — AGC mode (0=Off, 2=Slow, 3=Med, 4=Fast) ────────────────────

QString SmartCatProtocol::cmdGT(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return QString("GT%1;").arg(agcModeToZZ(a->agcMode()));
    a->setAgcMode(zzToAgcMode(arg));
    return {};
}

QString SmartCatProtocol::cmdZZGT(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return QString("ZZGT%1;").arg(agcModeToZZ(a->agcMode()));
    a->setAgcMode(zzToAgcMode(arg));
    return {};
}

// ── ZZAR — VFO A AGC threshold (0-100, 3-digit) ─────────────────────────────

QString SmartCatProtocol::cmdZZAR(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return "ZZAR" + fmt3(a->agcThreshold()) + ";";
    bool ok;
    int v = arg.toInt(&ok);
    if (!ok || v < 0 || v > 100) return "?;";
    a->setAgcThreshold(v);
    return {};
}

// ── ZZAS — VFO B AGC threshold (0-100, 3-digit) ─────────────────────────────

QString SmartCatProtocol::cmdZZAS(const QString& arg)
{
    SliceModel* b = sliceB();
    if (!b) return "?;";
    if (arg.isEmpty())
        return "ZZAS" + fmt3(b->agcThreshold()) + ";";
    bool ok;
    int v = arg.toInt(&ok);
    if (!ok || v < 0 || v > 100) return "?;";
    b->setAgcThreshold(v);
    return {};
}

// ── PC / ZZPC — RF power drive level (0-100, 3-digit) ───────────────────────

QString SmartCatProtocol::cmdPC(const QString& arg)
{
    TransmitModel& tx = m_model->transmitModel();
    if (arg.isEmpty())
        return "PC" + fmt3(tx.rfPower()) + ";";
    bool ok;
    int v = arg.toInt(&ok);
    if (!ok || v < 0 || v > 100) return "?;";
    tx.setRfPower(v);
    return {};
}

QString SmartCatProtocol::cmdZZPC(const QString& arg)
{
    TransmitModel& tx = m_model->transmitModel();
    if (arg.isEmpty())
        return "ZZPC" + fmt3(tx.rfPower()) + ";";
    bool ok;
    int v = arg.toInt(&ok);
    if (!ok || v < 0 || v > 100) return "?;";
    tx.setRfPower(v);
    return {};
}

// ── ZZMG — Transmitter mic gain (0-100, 3-digit) ────────────────────────────

QString SmartCatProtocol::cmdZZMG(const QString& arg)
{
    TransmitModel& tx = m_model->transmitModel();
    if (arg.isEmpty())
        return "ZZMG" + fmt3(tx.micLevel()) + ";";
    bool ok;
    int v = arg.toInt(&ok);
    if (!ok || v < 0 || v > 100) return "?;";
    tx.setMicLevel(v);
    return {};
}

// ── RG / ZZRG — VFO A RIT frequency get/set (±NNNNN Hz) ─────────────────────

QString SmartCatProtocol::cmdRG(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return "RG" + ritField(a->ritFreq()) + ";";
    bool ok;
    int hz = arg.toInt(&ok);
    if (!ok) return "?;";
    a->setRit(a->ritOn(), std::clamp(hz, -kRitMaxHz, kRitMaxHz));
    return {};
}

QString SmartCatProtocol::cmdZZRG(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return "ZZRG" + ritField(a->ritFreq()) + ";";
    bool ok;
    int hz = arg.toInt(&ok);
    if (!ok) return "?;";
    a->setRit(a->ritOn(), std::clamp(hz, -kRitMaxHz, kRitMaxHz));
    return {};
}

// ── RC / ZZRC — clear VFO A RIT frequency to 0 ──────────────────────────────

QString SmartCatProtocol::cmdRC()
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    a->setRit(a->ritOn(), 0);
    return {};
}

QString SmartCatProtocol::cmdZZRC()
{
    return cmdRC();
}

// ── RD / ZZRD — decrement VFO A RIT frequency ───────────────────────────────

QString SmartCatProtocol::cmdRD(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    int step = arg.isEmpty() ? 10 : qAbs(arg.toInt());
    a->setRit(a->ritOn(), std::clamp(a->ritFreq() - step, -kRitMaxHz, kRitMaxHz));
    return {};
}

QString SmartCatProtocol::cmdZZRD(const QString& arg)
{
    return cmdRD(arg);
}

// ── RU / ZZRU — increment VFO A RIT frequency ───────────────────────────────

QString SmartCatProtocol::cmdRU(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    int step = arg.isEmpty() ? 10 : qAbs(arg.toInt());
    a->setRit(a->ritOn(), std::clamp(a->ritFreq() + step, -kRitMaxHz, kRitMaxHz));
    return {};
}

QString SmartCatProtocol::cmdZZRU(const QString& arg)
{
    return cmdRU(arg);
}

// ── RT / ZZRT — VFO A RIT state (0/1) ───────────────────────────────────────

QString SmartCatProtocol::cmdRT(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return QString("RT%1;").arg(a->ritOn() ? 1 : 0);
    if (arg != "0" && arg != "1") return "?;";
    a->setRit(arg == "1", a->ritFreq());
    return {};
}

QString SmartCatProtocol::cmdZZRT(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return QString("ZZRT%1;").arg(a->ritOn() ? 1 : 0);
    if (arg != "0" && arg != "1") return "?;";
    a->setRit(arg == "1", a->ritFreq());
    return {};
}

// ── ZZRW — VFO B RIT frequency get/set ──────────────────────────────────────

QString SmartCatProtocol::cmdZZRW(const QString& arg)
{
    SliceModel* b = sliceB();
    if (!b) return "?;";
    if (arg.isEmpty())
        return "ZZRW" + ritField(b->ritFreq()) + ";";
    bool ok;
    int hz = arg.toInt(&ok);
    if (!ok) return "?;";
    b->setRit(b->ritOn(), std::clamp(hz, -kRitMaxHz, kRitMaxHz));
    return {};
}

// ── ZZRY — VFO B RIT state (0/1) ────────────────────────────────────────────

QString SmartCatProtocol::cmdZZRY(const QString& arg)
{
    SliceModel* b = sliceB();
    if (!b) return "?;";
    if (arg.isEmpty())
        return QString("ZZRY%1;").arg(b->ritOn() ? 1 : 0);
    if (arg != "0" && arg != "1") return "?;";
    b->setRit(arg == "1", b->ritFreq());
    return {};
}

// ── ZZXG — VFO A XIT frequency get/set ──────────────────────────────────────

QString SmartCatProtocol::cmdZZXG(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return "ZZXG" + ritField(a->xitFreq()) + ";";
    bool ok;
    int hz = arg.toInt(&ok);
    if (!ok) return "?;";
    a->setXit(a->xitOn(), std::clamp(hz, -kRitMaxHz, kRitMaxHz));
    return {};
}

// ── ZZXC — clear VFO A XIT frequency to 0 ───────────────────────────────────

QString SmartCatProtocol::cmdZZXC()
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    a->setXit(a->xitOn(), 0);
    return {};
}

// ── XT / ZZXS — VFO A XIT state (0/1) ───────────────────────────────────────

QString SmartCatProtocol::cmdXT(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return QString("XT%1;").arg(a->xitOn() ? 1 : 0);
    if (arg != "0" && arg != "1") return "?;";
    a->setXit(arg == "1", a->xitFreq());
    return {};
}

QString SmartCatProtocol::cmdZZXS(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return QString("ZZXS%1;").arg(a->xitOn() ? 1 : 0);
    if (arg != "0" && arg != "1") return "?;";
    a->setXit(arg == "1", a->xitFreq());
    return {};
}

// ── KS — CW keying speed (005-100 WPM, 3-digit) ─────────────────────────────

QString SmartCatProtocol::cmdKS(const QString& arg)
{
    TransmitModel& tx = m_model->transmitModel();
    if (arg.isEmpty())
        return "KS" + fmt3(tx.cwSpeed()) + ";";
    bool ok;
    int wpm = arg.toInt(&ok);
    if (!ok || wpm < 5 || wpm > 100) return "?;";
    tx.setCwSpeed(wpm);
    return {};
}

// ── PT — CW pitch frequency (100-999 Hz, 3-digit; values < 100 silently ignored)

QString SmartCatProtocol::cmdPT(const QString& arg)
{
    TransmitModel& tx = m_model->transmitModel();
    if (arg.isEmpty())
        return "PT" + fmt3(tx.cwPitch()) + ";";
    bool ok;
    int hz = arg.toInt(&ok);
    if (!ok) return "?;";
    if (hz < 100) return {};   // silently ignored per guide
    tx.setCwPitch(hz);
    return {};
}

// ── KY — send text to CWX keyer ──────────────────────────────────────────────
// Set:   "KY<sp><text>;" — P1 is 1 space byte, P2 is up to 24 chars of text
// Query: "KY;"           → "KY0;" (buffer empty) or "KY1;" (transmitting)

QString SmartCatProtocol::cmdKY(const QString& arg)
{
    if (arg.isEmpty())
        return QString("KY%1;").arg(m_model->cwxActive() ? 1 : 0);
    if (arg.size() < 2) return "?;";
    // arg[0] is the fixed P1 space; text starts at arg[1], max 24 chars
    const QString text = arg.mid(1).left(24);
    if (!text.isEmpty())
        m_model->cwxModel().send(text);
    return {};
}

// ── NB — Wide Noise Blanker state (0/1) ──────────────────────────────────────

QString SmartCatProtocol::cmdNB(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return QString("NB%1;").arg(a->nbOn() ? 1 : 0);
    if (arg != "0" && arg != "1") return "?;";
    a->setNb(arg == "1");
    return {};
}

// ── ZZNL — Wide Noise Blanker level (0-100, 3-digit) ────────────────────────

QString SmartCatProtocol::cmdZZNL(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return "ZZNL" + fmt3(a->nbLevel()) + ";";
    bool ok;
    int v = arg.toInt(&ok);
    if (!ok || v < 0 || v > 100) return "?;";
    a->setNbLevel(v);
    return {};
}

// ── ZZNR — Noise Reduction state (0/1) ───────────────────────────────────────

QString SmartCatProtocol::cmdZZNR(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return QString("ZZNR%1;").arg(a->nrOn() ? 1 : 0);
    if (arg != "0" && arg != "1") return "?;";
    a->setNr(arg == "1");
    return {};
}

// ── ZZBI — Binaural receive (0/1) ────────────────────────────────────────────

QString SmartCatProtocol::cmdZZBI(const QString& arg)
{
    if (!m_model) return "?;";
    if (arg.isEmpty())
        return QString("ZZBI%1;").arg(m_model->binauralRx() ? 1 : 0);
    if (arg != "0" && arg != "1") return "?;";
    m_model->setBinauralRx(arg == "1");
    return {};
}

// ── ZZDE — Diversity Receive (FLEX-6700) ─────────────────────────────────────

QString SmartCatProtocol::cmdZZDE(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return QString("ZZDE%1;").arg(a->diversity() ? 1 : 0);
    if (arg != "0" && arg != "1") return "?;";
    a->setDiversity(arg == "1");
    return {};
}

// ── SL / SH — DSP filter low / high cutoff (Kenwood SSB/FM codes) ────────────
//
// Kenwood encodes filter edges as an index into a fixed Hz table rather than
// raw Hz values.  We map from the slice's filterLow/filterHigh (signed Hz
// relative to carrier) to the nearest table entry, then round-trip back on set.
//
// SL table (low-edge): 00=10, 01=50, 02=100, 03=200, 04=300, 05=400,
//                      06=500, 07=600, 08=700, 09=800, 10=900, 11=1000 Hz
// SH table (high-edge): 00=1400,01=1600,02=1800,03=2000,04=2200,05=2400,
//                       06=2600,07=2800,08=3000,09=3400,10=4000,11=5000 Hz

static const int kSLHz[] = { 10, 50, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000 };
static const int kSHHz[] = { 1400, 1600, 1800, 2000, 2200, 2400, 2600, 2800, 3000, 3400, 4000, 5000 };
static constexpr int kSLCount = 12;
static constexpr int kSHCount = 12;

static int nearestIndex(const int* table, int count, int hz)
{
    int best = 0;
    int bestDist = qAbs(table[0] - hz);
    for (int i = 1; i < count; ++i) {
        int d = qAbs(table[i] - hz);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

QString SmartCatProtocol::cmdSL(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty()) {
        int sl_hz = qMin(qAbs(a->filterLow()), qAbs(a->filterHigh()));
        int code  = nearestIndex(kSLHz, kSLCount, sl_hz);
        return QString("SL%1;").arg(code, 2, 10, QChar('0'));
    }
    bool ok;
    int code = arg.toInt(&ok);
    if (!ok || code < 0 || code >= kSLCount) return "?;";
    int sl_hz = kSLHz[code];
    // preserve the sign: low edge is negative for LSB, positive for USB
    int sh_hz = qMax(qAbs(a->filterLow()), qAbs(a->filterHigh()));
    if (a->filterLow() < 0)
        a->setFilterWidth(-sh_hz, -sl_hz);
    else
        a->setFilterWidth(sl_hz, sh_hz);
    return {};
}

QString SmartCatProtocol::cmdSH(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty()) {
        int sh_hz = qMax(qAbs(a->filterLow()), qAbs(a->filterHigh()));
        int code  = nearestIndex(kSHHz, kSHCount, sh_hz);
        return QString("SH%1;").arg(code, 2, 10, QChar('0'));
    }
    bool ok;
    int code = arg.toInt(&ok);
    if (!ok || code < 0 || code >= kSHCount) return "?;";
    int sh_hz = kSHHz[code];
    int sl_hz = qMin(qAbs(a->filterLow()), qAbs(a->filterHigh()));
    if (a->filterLow() < 0)
        a->setFilterWidth(-sh_hz, -sl_hz);
    else
        a->setFilterWidth(sl_hz, sh_hz);
    return {};
}

// ── ZZFR — toggle active VFO (swaps m_vfoA ↔ m_vfoB) ───────────────────────

QString SmartCatProtocol::cmdZZFR()
{
    std::swap(m_vfoA, m_vfoB);
    return {};
}

// ── SQ — squelch level (P1=main/sub selector, P2=000-255) ────────────────────
//
// MacLoggerDX polls SQ0; to detect squelch state.
// P1: 0=main, 1=sub.  We only support main (slice A).
// P2: 000-255 squelch level.

QString SmartCatProtocol::cmdSQ(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    // Read: SQ; or SQ0; (P1 channel selector only)
    if (arg.isEmpty() || arg.size() == 1)
        return QString("SQ0%1;").arg(a->squelchLevel(), 3, 10, QChar('0'));
    // Set: SQP1LLL; — P1 is 1 char (0=main, 1=sub), LLL is 3-digit level
    if (arg.size() < 4) return "?;";
    // arg[0] is the P1 channel selector (ignore, we only have main)
    bool ok;
    int level = arg.mid(1).toInt(&ok);
    if (!ok || level < 0 || level > 255) return "?;";
    a->setSquelch(a->squelchOn(), level);
    return {};
}

// ── NR — Noise Reduction ON/OFF (0=off, 1=NR1, 2=NR2) ──────────────────────
//
// Kenwood TS-2000 NR command maps 0/1/2. SmartSDR only has a binary NR
// on/off via nrOn()/setNr(). Treat 0=off, 1 or 2 = on.

QString SmartCatProtocol::cmdNR(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return QString("NR%1;").arg(a->nrOn() ? 1 : 0);
    if (arg != "0" && arg != "1" && arg != "2") return "?;";
    a->setNr(arg != "0");
    return {};
}

// ── NL — Noise Blanker level (001-010 per TS-2000 spec) ──────────────────────
//
// SmartSDR stores NB level 0-100.  We scale: NB level 1-10 maps to 0-100
// linearly (each TS-2000 step ≈ 10 SmartSDR units).

QString SmartCatProtocol::cmdNL(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty()) {
        // Map SmartSDR 0-100 → TS-2000 001-010
        int ssdr = a->nbLevel();                        // 0-100
        int ts   = qBound(1, (ssdr + 5) / 10, 10);    // round to 1-10
        return QString("NL%1;").arg(ts, 3, 10, QChar('0'));
    }
    bool ok;
    int ts = arg.toInt(&ok);
    if (!ok || ts < 0 || ts > 10) return "?;";
    // Map TS-2000 0-10 → SmartSDR 0-100
    int ssdr = qBound(0, ts * 10, 100);
    a->setNbLevel(ssdr);
    return {};
}

// ── NT — Auto Notch (ANF) ON/OFF ─────────────────────────────────────────────

QString SmartCatProtocol::cmdNT(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty())
        return QString("NT%1;").arg(a->anfOn() ? 1 : 0);
    if (arg != "0" && arg != "1") return "?;";
    a->setAnf(arg == "1");
    return {};
}

// ── RA — Attenuator (00=off, 01-99=on) ───────────────────────────────────────
//
// SmartSDR FlexRadios do not expose a discrete attenuator switch via the
// model layer. RF gain is a separate continuous control (RG / rfGain).
// Return a fixed "ATT off" (RA00) and silently accept sets to avoid ?; errors
// from logging software that queries RA.

QString SmartCatProtocol::cmdRA(const QString& arg)
{
    if (arg.isEmpty()) return QStringLiteral("RA0000;");
    // Accept set silently (P1 is 2 digits, P2 is 2 digits for sub — total 4)
    return {};
}

// ── PA — Pre-amplifier ON/OFF ─────────────────────────────────────────────────
//
// SmartSDR does not expose a dedicated pre-amplifier enable flag via the
// CAT-accessible model. Return fixed "preamp off" and accept sets silently.
// PA answer format: P1=main preamp, P2=sub preamp.

QString SmartCatProtocol::cmdPA(const QString& arg)
{
    if (arg.isEmpty()) return QStringLiteral("PA00;");
    return {};
}

// ── MG — Microphone gain (000-100, 3-digit) ─────────────────────────────────
//
// Same data as ZZMG but without the ZZ prefix (plain Kenwood command).

QString SmartCatProtocol::cmdMG(const QString& arg)
{
    TransmitModel& tx = m_model->transmitModel();
    if (arg.isEmpty())
        return "MG" + fmt3(tx.micLevel()) + ";";
    bool ok;
    int v = arg.toInt(&ok);
    if (!ok || v < 0 || v > 100) return "?;";
    tx.setMicLevel(v);
    return {};
}

// ── RL — Noise Reduction level (00-09 per TS-2000 spec) ──────────────────────
//
// SmartSDR stores NR level 0-100.  TS-2000 uses 00 (AUTO/0ms) through 09.
// We map linearly: TS-2000 00-09 ↔ SmartSDR 0-100.

QString SmartCatProtocol::cmdRL(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    if (arg.isEmpty()) {
        int ssdr = a->nrLevel();                      // 0-100
        int ts   = qBound(0, (ssdr + 5) / 10, 9);   // round to 0-9
        return QString("RL%1;").arg(ts, 2, 10, QChar('0'));
    }
    bool ok;
    int ts = arg.toInt(&ok);
    if (!ok || ts < 0 || ts > 9) return "?;";
    int ssdr = qBound(0, ts * 11, 100);   // 9*11=99 ≈ 100
    a->setNrLevel(ssdr);
    return {};
}

// ── RM — Meter reading (stub) ─────────────────────────────────────────────────
//
// P1 selects meter: 0=unselected, 1=SWR, 2=COMP, 3=ALC.
// P2 is the dot-count reading 0000-0030.
// The CAT protocol layer has no access to live meter telemetry; return a
// fixed zero-dots value and silently accept set commands.

QString SmartCatProtocol::cmdRM(const QString& arg)
{
    // Query: RM; or RM P1; — return zero reading; no live meter data.
    if (arg.isEmpty() || arg.size() == 1)
        return QString("RM%1%2;").arg(arg.isEmpty() ? QChar('0') : arg[0])
                                  .arg(0, 4, 10, QChar('0'));
    return {};
}

// ── LK — Key lock status (stub) ──────────────────────────────────────────────
//
// SDRs have no front-panel key lock. Return "unlocked" (LK00) and accept sets.

QString SmartCatProtocol::cmdLK(const QString& arg)
{
    if (arg.isEmpty()) return QStringLiteral("LK00;");
    return {};
}

// ── TY — Firmware type (stub) ────────────────────────────────────────────────
//
// P1 is reserved (space), P2: 0=overseas, 1=JP100W, 2=JP20W.
// We always report overseas type.

QString SmartCatProtocol::cmdTY(const QString& arg)
{
    if (arg.isEmpty()) return QStringLiteral("TY  0;");
    return {};
}

// ── BY — Busy indicator (squelch open) ───────────────────────────────────────
//
// P1=main busy (0=not busy/squelch closed, 1=busy/squelch open).
// P2=sub busy (0 — no sub-receiver in SDR mode).
// We treat the squelch state as the "busy" indicator.

QString SmartCatProtocol::cmdBY(const QString& /*arg*/)
{
    SliceModel* a = sliceA();
    // If no slice or squelch is off (always open), report busy=1.
    // If squelch is on and active (signal present), report busy=1, else 0.
    // SmartSDR doesn't directly expose squelch-open state, only the
    // threshold. Return 0 (not busy) as a safe default.
    int busy = 0;
    if (a && a->squelchOn() && a->squelchLevel() == 0)
        busy = 1;  // squelch at 0 means always open = busy
    return QStringLiteral("BY") + QChar('0' + busy) + "0;";
}

// ── OI — Opposite IF (VFO B composite status, same format as IF) ─────────────
//
// OI returns the same 37-character body as IF but for the "opposite" VFO (B).
// Used by some logging apps to read split TX frequency without switching VFOs.

QString SmartCatProtocol::cmdOI()
{
    SliceModel* b = sliceB();
    SliceModel* a = sliceA();
    // Fall back to slice A if there is no VFO B
    SliceModel* s = b ? b : a;
    if (!s) return "?;";

    QString body;
    body += freqField(s->frequency());
    body += QStringLiteral("00010");
    body += QStringLiteral("+00000");
    body += '0';
    body += '0';
    body += QStringLiteral("00");
    body += m_model->isRadioTransmitting() ? '1' : '0';
    body += modeToKenwood(s->mode());
    body += '0';
    body += '0';
    body += m_splitEnabled ? '1' : '0';
    body += '0';
    body += QStringLiteral("00");
    body += '0';

    return "OI" + body + ";";
}

// ── UP — VFO frequency step up ────────────────────────────────────────────────
//
// Emulates the microphone UP key — steps the VFO A frequency up by one step.
// P1 is 2-digit step count (optional; default 1 step).
// Uses the slice's stepHz() for the step size.

QString SmartCatProtocol::cmdUP(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    int steps = 1;
    if (!arg.isEmpty()) {
        bool ok;
        int n = arg.toInt(&ok);
        if (ok && n > 0) steps = n;
    }
    double stepMhz = static_cast<double>(a->stepHz()) / 1e6;
    a->setFrequency(a->frequency() + stepMhz * steps);
    return {};
}

// ── DN — VFO frequency step down ─────────────────────────────────────────────
//
// Emulates the microphone DN key — steps the VFO A frequency down by one step.

QString SmartCatProtocol::cmdDN(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";
    int steps = 1;
    if (!arg.isEmpty()) {
        bool ok;
        int n = arg.toInt(&ok);
        if (ok && n > 0) steps = n;
    }
    double stepMhz = static_cast<double>(a->stepHz()) / 1e6;
    a->setFrequency(a->frequency() - stepMhz * steps);
    return {};
}

// ── FW — DSP filter width ─────────────────────────────────────────────────────
//
// TS-2000 FW command sets the DSP receive filter width in Hz.
// Valid only in CW (50-2000 Hz) and FM/AM (0=narrow, 1=wide) modes.
// In SSB mode the spec says to use SL/SH instead — we return ?; for SSB.
// CW: we map the supplied Hz to filter edges ±(hz/2) around the carrier.
// FM/AM: 0=narrow (±2500 Hz), 1=wide (±5000 Hz).

static const int kFwCwValues[] = { 50, 80, 100, 150, 200, 300, 400, 500, 600, 1000, 2000 };
static constexpr int kFwCwCount = 11;

static int nearestCwFw(int hz)
{
    int best = kFwCwValues[0];
    int bestDist = qAbs(best - hz);
    for (int i = 1; i < kFwCwCount; ++i) {
        int d = qAbs(kFwCwValues[i] - hz);
        if (d < bestDist) { bestDist = d; best = kFwCwValues[i]; }
    }
    return best;
}

QString SmartCatProtocol::cmdFW(const QString& arg)
{
    SliceModel* a = sliceA();
    if (!a) return "?;";

    const QString mode = a->mode();
    const bool isCw   = (mode == "CW" || mode == "CWL");
    const bool isFm   = (mode == "FM" || mode == "NFM" || mode == "DFM");
    const bool isAm   = (mode == "AM" || mode == "SAM");

    // SSB and digital modes: FW is not valid
    if (!isCw && !isFm && !isAm) return "?;";

    if (arg.isEmpty()) {
        if (isCw) {
            // Compute current filter width in Hz
            int bw = qAbs(a->filterHigh()) + qAbs(a->filterLow());
            int nearest = nearestCwFw(bw);
            return QString("FW%1;").arg(nearest, 4, 10, QChar('0'));
        } else {
            // FM/AM: 0=narrow (filter width ≤ 5000), 1=wide
            int bw = qAbs(a->filterHigh()) + qAbs(a->filterLow());
            return QString("FW%1;").arg(bw <= 5000 ? "0000" : "0001");
        }
    }

    bool ok;
    int hz = arg.toInt(&ok);
    if (!ok || hz < 0) return "?;";

    if (isCw) {
        int bw = nearestCwFw(hz);
        // Symmetric filter around 0 Hz: low = -bw/2, high = +bw/2
        a->setFilterWidth(-(bw / 2), bw / 2);
    } else {
        // FM/AM: 0=narrow, 1=wide
        if (hz == 0)
            a->setFilterWidth(-2500, 2500);
        else
            a->setFilterWidth(-5000, 5000);
    }
    return {};
}

// ── ZZFI / ZZFJ — VFO A/B DSP Filter Index ──────────────────────────────────
// Index 00 = widest, 07 = narrowest. Boundaries from CAT guide v4.1.5 §3.3.9.

static const int kZZFIThresh[] = { 3300, 2900, 2700, 2400, 2100, 1800, 1500 };
static const int kZZFIRepBw[]  = { 3600, 3100, 2800, 2550, 2250, 1950, 1650, 1200 };
static constexpr int kZZFICount = 8;

static int bwToZZFIIndex(int bwHz)
{
    for (int i = 0; i < kZZFICount - 1; ++i)
        if (bwHz > kZZFIThresh[i])
            return i;
    return kZZFICount - 1;
}

static QString zzfiQuery(SliceModel* s)
{
    if (!s) return "?;";
    int bw = s->filterHigh() - s->filterLow();
    return QString("ZZFI%1;").arg(bwToZZFIIndex(qAbs(bw)), 2, 10, QChar('0'));
}

static QString zzfijSet(SliceModel* s, const QString& arg, const QString& prefix)
{
    if (!s) return "?;";
    bool ok;
    int idx = arg.toInt(&ok);
    if (!ok || idx < 0 || idx >= kZZFICount) return "?;";
    int newBw  = kZZFIRepBw[idx];
    int center = (s->filterLow() + s->filterHigh()) / 2;
    s->setFilterWidth(center - newBw / 2, center + newBw / 2);
    Q_UNUSED(prefix)
    return {};
}

QString SmartCatProtocol::cmdZZFI(const QString& arg)
{
    SliceModel* a = sliceA();
    if (arg.isEmpty()) {
        QString r = zzfiQuery(a);
        return r;
    }
    return zzfijSet(a, arg, "ZZFI");
}

QString SmartCatProtocol::cmdZZFJ(const QString& arg)
{
    SliceModel* b = sliceB();
    if (arg.isEmpty()) {
        if (!b) return zzfiQuery(sliceA()).replace("ZZFI", "ZZFJ");
        QString r = zzfiQuery(b);
        return r.replace("ZZFI", "ZZFJ");
    }
    return zzfijSet(b ? b : sliceA(), arg, "ZZFJ");
}

} // namespace AetherSDR
