#include "Kpa500Protocol.h"

#include <QStringList>

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
    for (const char byte : bytes) {
        // A newer start marker replaces a truncated frame, including within
        // one read. Limit each pending frame, not the size of a serial read.
        if (byte == '^') {
            if (!m_buf.isEmpty() && m_error) { m_error(QStringLiteral("Truncated frame; resynchronizing")); }
            m_buf = QByteArrayLiteral("^");
            continue;
        }
        if (m_buf.isEmpty()) {
            continue;
        }
        if (m_buf.size() >= kMaxFrameBytes) {
            if (m_error) { m_error(QStringLiteral("Oversized frame discarded")); }
            m_buf.clear();
            continue;
        }
        if (static_cast<unsigned char>(byte) < 0x20 || static_cast<unsigned char>(byte) > 0x7e) {
            if (m_error) { m_error(QStringLiteral("Non-printable frame discarded")); }
            m_buf.clear();
            continue;
        }
        if (byte != ';') {
            m_buf.append(byte);
            continue;
        }

        const QByteArray body = m_buf.mid(1);
        m_buf.clear();
        int tokenLength = 0;
        while (tokenLength < body.size()
               && ((body[tokenLength] >= 'A' && body[tokenLength] <= 'Z')
                   || (body[tokenLength] >= 'a' && body[tokenLength] <= 'z'))) {
            ++tokenLength;
        }
        if (tokenLength < 2 || tokenLength > 3) {
            if (m_error) { m_error(QStringLiteral("Malformed command token discarded")); }
            continue;
        }
        if (m_cb) {
            m_cb(QString::fromLatin1(body.left(tokenLength)).toUpper(),
                 QString::fromLatin1(body.mid(tokenLength)));
        }
    }
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

static std::optional<int> decimalField(const QString& text, int minDigits, int maxDigits)
{
    if (text.size() < minDigits || text.size() > maxDigits) {
        return std::nullopt;
    }
    for (const QChar c : text) {
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) {
            return std::nullopt;
        }
    }
    return text.toInt();
}

bool applyMessage(const QString& cmd, const QString& arg, Status& status, bool* accepted)
{
    bool changed = false;
    bool valid = false;

    if (cmd == QLatin1String("OS") || cmd == QLatin1String("ON") || cmd == QLatin1String("SP")) {
        const std::optional<int> value = decimalField(arg, 1, 1);
        if (value && *value <= 1) {
            valid = true;
            changed = cmd == QLatin1String("OS")
                ? assignIfChanged(status.operate, *value == 1)
                : cmd == QLatin1String("ON") ? assignIfChanged(status.powerOn, *value == 1)
                : assignIfChanged(status.speakerOn, *value == 1);
        }
    } else if (cmd == QLatin1String("BN")) {
        const std::optional<int> value = decimalField(arg, 2, 2);
        if (value && *value <= 10) {
            valid = true;
            changed = assignIfChanged(status.band, *value);
        }
    } else if (cmd == QLatin1String("WS") || cmd == QLatin1String("VI")) {
        // Rev. A2 specifies two three-digit decimal fields separated by a space.
        const QStringList parts = arg.split(QLatin1Char(' '));
        if (parts.size() == 2) {
            const std::optional<int> first = decimalField(parts[0], 3, 3);
            const std::optional<int> second = decimalField(parts[1], 3, 3);
            if (first && second) {
                if (cmd == QLatin1String("VI")) {
                    valid = true;
                    changed |= assignIfChanged(status.paVoltageV, *first / 10.0f);
                    changed |= assignIfChanged(status.paCurrentA, *second / 10.0f);
                } else if (*second == 0 || (*second >= 10 && *second <= 990)) {
                    valid = true;
                    changed |= assignIfChanged(status.forwardPowerW, static_cast<float>(*first));
                    if (*second == 0) {
                        changed |= status.swr.has_value();
                        status.swr.reset();
                    } else {
                        changed |= assignIfChanged(status.swr, *second / 10.0f);
                    }
                }
            }
        }
    } else if (cmd == QLatin1String("TM")) {
        const std::optional<int> value = decimalField(arg, 3, 3);
        if (value && *value <= 150) {
            valid = true;
            changed = assignIfChanged(status.paTemperatureC, static_cast<float>(*value));
        }
    } else if (cmd == QLatin1String("FL")) {
        const std::optional<int> value = decimalField(arg, 2, 2);
        if (value) {
            valid = true;
            changed = assignIfChanged(status.faultCode, *value);
        }
    } else if (cmd == QLatin1String("FC")) {
        // Rev. A2 documents one digit; firmware 1.54 also replies with 00–06.
        const std::optional<int> value = decimalField(arg, 1, 2);
        if (value && *value <= 6) {
            valid = true;
            changed = assignIfChanged(status.fanMinSpeed, *value);
        }
    } else if (cmd == QLatin1String("RVM")) {
        const QStringList parts = arg.split(QLatin1Char('.'));
        if (parts.size() == 2 && decimalField(parts[0], 1, 2)
            && decimalField(parts[1], 2, 2)) {
            valid = true;
            changed = assignIfChanged(status.firmwareVersion, arg);
        }
    } else if (cmd == QLatin1String("SN")) {
        if (decimalField(arg, 5, 5)) {
            valid = true;
            changed = assignIfChanged(status.serialNumber, arg);
        }
    }

    if (accepted) {
        *accepted = valid;
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

QByteArray buildPowerOn() { return QByteArrayLiteral("P"); }

QByteArray buildSetBand(int band)
{
    if (band < 0 || band > 10) { return {}; }
    return "^BN" + QByteArray::number(band).rightJustified(2, '0') + ';';
}

QByteArray buildSetSpeaker(bool on)
{
    return on ? QByteArrayLiteral("^SP1;") : QByteArrayLiteral("^SP0;");
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
    // One command every 200 ms, so each field is refreshed every 800 ms.
    static const QStringList kFast{
        QStringLiteral("WS"),  // power + SWR
        QStringLiteral("OS"),  // operate/standby
        QStringLiteral("BN"),  // confirmed amplifier band
        QStringLiteral("FL"),  // faults remain visible in mini mode
    };
    return kFast;
}

const QStringList& slowPollCommands()
{
    // Config/status that rarely changes — one command rotated per 500 ms.
    static const QStringList kSlow{
        QStringLiteral("TM"),  // PA temperature
        QStringLiteral("VI"),  // voltage + current
        QStringLiteral("ON"),  // power status
        QStringLiteral("FC"),  // fan minimum speed
        QStringLiteral("SP"),  // fault speaker; queried only while confirmed on
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
