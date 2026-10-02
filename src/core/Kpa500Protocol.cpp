#include "Kpa500Protocol.h"

#include <QStringList>

#include <cctype>

namespace AetherSDR {
namespace Kpa500 {

// ── Band names ────────────────────────────────────────────────────────────────

QString bandName(int band)
{
    // Source: KPA500 Programmer's Reference Rev. A2, §^BN.
    static const char* const kNames[] = {
        "160m", "80m", "60m", "40m", "30m",
        "20m",  "17m", "15m", "12m", "10m", "6m"
    };
    if (band < 0 || band > 10)
        return QStringLiteral("?m");
    return QLatin1String(kNames[band]);
}

// ── Frame parser ──────────────────────────────────────────────────────────────

void FrameParser::feed(const QByteArray& bytes)
{
    // Guard against unbounded growth. Append first so a chunk that would
    // complete a buffered partial frame is not discarded before it arrives.
    m_buf.append(bytes);
    if (m_buf.size() > kMaxFrameBytes * 4)
        m_buf.clear();

    while (true) {
        int semi = m_buf.indexOf(';');
        if (semi < 0)
            break;

        // Extract everything up to and including the semicolon.
        QByteArray raw = m_buf.left(semi + 1);
        m_buf.remove(0, semi + 1);

        // Discard anything before the first '^' (includes null-command echo).
        int caret = raw.indexOf('^');
        if (caret < 0)
            continue;

        // Body is between '^' (exclusive) and ';' (exclusive).
        QByteArray body = raw.mid(caret + 1, raw.size() - caret - 2);
        if (body.isEmpty())
            continue;

        // Split: cmd = leading letters (upper-cased), arg = rest (trimmed).
        // The amp always responds upper-case, but toUpper() is cheap insurance.
        int i = 0;
        while (i < body.size() && isalpha(static_cast<unsigned char>(body[i])))
            ++i;

        QString cmd = QString::fromLatin1(body.left(i)).toUpper();
        QString arg = QString::fromLatin1(body.mid(i)).trimmed();

        if (cmd.isEmpty() || cmd.size() > 3)
            continue;  // Malformed command token — skip

        if (m_cb)
            m_cb(cmd, arg);
    }

    // If a '^' frame arrived without its ';', cap the buffer so we don't
    // accumulate indefinitely on a chatty/wrong peer.
    if (m_buf.size() > kMaxFrameBytes)
        m_buf.clear();
}

// ── Message decode ────────────────────────────────────────────────────────────

// Helper: update an optional field, tracking whether anything changed.
template<typename T>
static bool assignIfChanged(std::optional<T>& field, T value)
{
    if (field && *field == value)
        return false;
    field = value;
    return true;
}

bool applyMessage(const QString& cmd, const QString& arg, Status& status)
{
    bool changed = false;

    if (cmd == QLatin1String("OS")) {
        // ^OSn; — 0=standby, 1=operate.
        bool ok;
        int v = arg.toInt(&ok);
        if (ok && (v == 0 || v == 1))
            changed |= assignIfChanged(status.operate, v == 1);

    } else if (cmd == QLatin1String("ON")) {
        // ^ONn; — RSP is always n=1 (on). No response when off — we never
        // receive a '0' here; cleared by the connection on loss instead.
        bool ok;
        int v = arg.toInt(&ok);
        if (ok && v == 1)
            changed |= assignIfChanged(status.powerOn, true);

    } else if (cmd == QLatin1String("BN")) {
        // ^BNnn; — band index 0–10.
        bool ok;
        int v = arg.toInt(&ok);
        if (ok && v >= 0 && v <= 10)
            changed |= assignIfChanged(status.band, v);

    } else if (cmd == QLatin1String("WS")) {
        // ^WSppp sss; — space-separated, two fields.
        // ppp = forward power watts (integer 0–999, no implied decimal).
        // sss = SWR with implied decimal after 2nd digit (so "015" → 1.5).
        // Source: KPA500 Programmer's Reference Rev. A2, §^WS.
        const QStringList parts = arg.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() == 2) {
            bool ok1, ok2;
            int ppp = parts[0].toInt(&ok1);
            int sss = parts[1].toInt(&ok2);
            if (ok1 && ok2 && ppp >= 0 && ppp <= 999 && sss >= 0) {
                changed |= assignIfChanged(status.forwardPowerW, static_cast<float>(ppp));
                changed |= assignIfChanged(status.swr, sss / 10.0f);
            }
        }

    } else if (cmd == QLatin1String("VI")) {
        // ^VIvvv iii; — space-separated, two fields.
        // vvv = PA voltage, iii = PA current. Both have implied decimal after
        // 2nd digit, so raw "120" → 12.0 V and raw "053" → 5.3 A.
        // Source: KPA500 Programmer's Reference Rev. A2, §^VI.
        const QStringList parts = arg.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() == 2) {
            bool ok1, ok2;
            int vvv = parts[0].toInt(&ok1);
            int iii = parts[1].toInt(&ok2);
            if (ok1 && ok2 && vvv >= 0 && iii >= 0) {
                changed |= assignIfChanged(status.paVoltageV, vvv / 10.0f);
                changed |= assignIfChanged(status.paCurrentA, iii / 10.0f);
            }
        }

    } else if (cmd == QLatin1String("TM")) {
        // ^TMnnn; — temperature 0–150 °C.
        bool ok;
        int v = arg.toInt(&ok);
        if (ok && v >= 0 && v <= 150)
            changed |= assignIfChanged(status.paTemperatureC, static_cast<float>(v));

    } else if (cmd == QLatin1String("FL")) {
        // ^FLnn; RSP — decimal fault code, 00 = no fault.
        // Note: ^FLC; is a SET (clear command) not a RSP — it is never echoed
        // back by the amp, so we will never receive cmd=="FL" arg=="C" here.
        // Source: KPA500 Programmer's Reference Rev. A2, §^FL.
        bool ok;
        int v = arg.toInt(&ok);
        if (ok && v >= 0 && v <= 99)
            changed |= assignIfChanged(status.faultCode, v);

    } else if (cmd == QLatin1String("FC")) {
        // ^FCn; — fan minimum speed 0–6.
        // Source: KPA500 Programmer's Reference Rev. A2, §^FC.
        bool ok;
        int v = arg.toInt(&ok);
        if (ok && v >= 0 && v <= 6)
            changed |= assignIfChanged(status.fanMinSpeed, v);

    } else if (cmd == QLatin1String("RVM")) {
        // ^RVMnn.nn; — firmware version, one-shot on connect.
        QString ver = arg.trimmed();
        if (!ver.isEmpty())
            changed |= assignIfChanged(status.firmwareVersion, ver);

    } else if (cmd == QLatin1String("SN")) {
        // ^SNnnnnn; — serial number, one-shot on connect.
        QString sn = arg.trimmed();
        if (!sn.isEmpty())
            changed |= assignIfChanged(status.serialNumber, sn);
    }

    return changed;
}

// ── Command builders ──────────────────────────────────────────────────────────

QByteArray buildQuery(const char* cmd)
{
    return QByteArray("^") + cmd + ';';
}

QByteArray buildOperate()
{
    return QByteArrayLiteral("^OS1;");
}

QByteArray buildStandby()
{
    return QByteArrayLiteral("^OS0;");
}

QByteArray buildClearFault()
{
    // Source: KPA500 Programmer's Reference Rev. A2, §^FL:
    // "SET format: ^FLC; clears the current fault."
    return QByteArrayLiteral("^FLC;");
}

QByteArray buildPowerOff()
{
    // Source: KPA500 Programmer's Reference Rev. A2, §^ON:
    // "SET format: ^ON0; turns the KPA500 off."
    return QByteArrayLiteral("^ON0;");
}

QByteArray buildPowerOn()
{
    // Source: KPA500 Programmer's Reference Rev. A2, §^ON.
    // Works over the normal serial port (confirmed by KPA500 Remote and
    // third-party applications). No UI wired in this PR — see header.
    return QByteArrayLiteral("^ON1;");
}

QByteArray buildSetFanSpeed(int n)
{
    // Source: KPA500 Programmer's Reference Rev. A2, §^FC:
    // "SET/RSP format: ^FCn; where n = fan minimum speed with range from 0 (off) to 6 (high)."
    n = qBound(0, n, 6);
    return QByteArray("^FC") + QByteArray::number(n) + ';';
}

QByteArray buildNullCommand()
{
    // Source: §Command Format — "The KPA500 will respond to a null command,
    // containing only a ';' by echoing the ';' character."
    return QByteArrayLiteral(";");
}

// ── Poll schedules ────────────────────────────────────────────────────────────

const QStringList& fastPollCommands()
{
    // Values that change on every TX/RX cycle or QSY — queried every ~200 ms.
    // BN is here (not slow) so a band change shows within one fast-poll cycle
    // rather than waiting up to 10 s for the slow rotation to reach it.
    static const QStringList kFast{
        QStringLiteral("WS"),  // power + SWR
        QStringLiteral("OS"),  // operate/standby
        QStringLiteral("BN"),  // band — fast so QSY is reflected immediately
    };
    return kFast;
}

const QStringList& slowPollCommands()
{
    // Config/status that rarely changes — one command rotated per ~2 s.
    static const QStringList kSlow{
        QStringLiteral("TM"),  // PA temperature
        QStringLiteral("VI"),  // voltage + current
        QStringLiteral("FL"),  // fault code
        QStringLiteral("ON"),  // power status
        QStringLiteral("FC"),  // fan minimum speed
    };
    return kSlow;
}

const QStringList& connectCommands()
{
    // One-shot after first connect: firmware version + serial number.
    static const QStringList kConnect{
        QStringLiteral("RVM"),
        QStringLiteral("SN"),
    };
    return kConnect;
}

}  // namespace Kpa500
}  // namespace AetherSDR
