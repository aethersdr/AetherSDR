#include "Kpa1500Protocol.h"

#include <algorithm>

namespace AetherSDR {
namespace Kpa1500 {

namespace {

// Reply scaling, per the Programming Reference: ^SW "is expressed in
// tenths. ^SW123; is 12.3 : 1"; ^PWF/^PWR are whole watts. A payload with an
// explicit decimal point is taken at face value instead.
constexpr float kSwrDivisor = 10.0f;
constexpr float kPowerDivisor = 1.0f;

// Every command in the reference is two letters except these. Payloads may
// start with a letter (^AMI;, ^FLB0;), so the token is matched by name: a
// leading run of letters would read ^FLB0; as a command "FLB".
constexpr const char* kThreeLetterCommands[] = {
    "BRP", "BRX", "PWD", "PWF", "PWI", "PWR", "RVM", "STA", "STB", "STN", "STS", "VMH",
};

bool isUpperLetter(QChar c)
{
    return c >= QLatin1Char('A') && c <= QLatin1Char('Z');
}

// Command token for building a frame: 2 letters, or one of the 3-letter
// commands above. Anything else would put a frame on the wire the amp
// cannot parse.
bool isValidCommandToken(const QString& cmd)
{
    if (cmd.size() == 2) {
        return isUpperLetter(cmd.at(0)) && isUpperLetter(cmd.at(1));
    }
    if (cmd.size() == 3) {
        for (const char* known : kThreeLetterCommands) {
            if (cmd == QLatin1String(known)) {
                return true;
            }
        }
    }
    return false;
}

// Splits "PWF1200" into ("PWF", "1200") and "AMI" into ("AM", "I"). Returns
// false for a body that does not start with a command (Principle VII).
bool splitBody(const QString& body, QString& cmd, QString& arg)
{
    // Message::cmd is always upper-case, so a lower-case token is normalized
    // here rather than rejected.
    const QString head = body.left(3).toUpper();
    if (head.size() < 2 || !isUpperLetter(head.at(0)) || !isUpperLetter(head.at(1))) {
        return false;
    }
    const int len = isValidCommandToken(head) ? 3 : 2;
    cmd = head.left(len);
    arg = body.mid(len);
    return arg.size() <= MessageParser::kMaxArgChars;
}

std::optional<int> decodeInt(const QString& arg)
{
    if (arg.isEmpty()) {
        return std::nullopt;
    }
    bool ok = false;
    const int n = arg.toInt(&ok);
    return ok ? std::optional<int>(n) : std::nullopt;
}

// Numeric payload with an explicit-decimal-point escape hatch — see
// kSwrDivisor's comment. Negative results are rejected: none of the fields
// this decodes (power, SWR) has a meaningful negative value, and letting
// one through would drive a gauge backwards.
std::optional<float> decodeScaled(const QString& arg, float divisor)
{
    if (arg.isEmpty()) {
        return std::nullopt;
    }
    bool ok = false;
    float v = 0.0f;
    if (arg.contains(QLatin1Char('.'))) {
        v = arg.toFloat(&ok);
    } else {
        const int raw = arg.toInt(&ok);
        v = ok ? static_cast<float>(raw) / divisor : 0.0f;
    }
    if (!ok || v < 0.0f) {
        return std::nullopt;
    }
    return v;
}

// Boolean payloads are exactly "0"/"1". Anything else is rejected rather
// than coerced, so a malformed frame cannot flip OPERATE.
std::optional<bool> decodeBool(const QString& arg)
{
    if (arg == QLatin1String("1")) {
        return true;
    }
    if (arg == QLatin1String("0")) {
        return false;
    }
    return std::nullopt;
}

// ^FLhh: two hex digits ("90", "B0", "C1").
std::optional<int> decodeFault(const QString& arg)
{
    if (arg.size() != 2) {
        return std::nullopt;
    }
    bool ok = false;
    const int code = arg.toInt(&ok, 16);
    return ok ? std::optional<int>(code) : std::nullopt;
}

// ^TQn: 0 not keyed; 1, 2, 3 keyed (by ^TX, with or without KEY IN).
std::optional<bool> decodeKeyState(const QString& arg)
{
    const auto n = decodeInt(arg);
    if (!n || *n < 0 || *n > 3 || arg.size() != 1) {
        return std::nullopt;
    }
    return *n != 0;
}

// Assigns and reports whether the value actually moved, so applyMessage()
// can tell the caller there is something new to repaint.
template <typename T>
bool assignIfChanged(std::optional<T>& field, const std::optional<T>& value)
{
    if (!value || field == value) {
        return false;
    }
    field = value;
    return true;
}

}  // namespace

QByteArray buildMessage(const QString& cmd, const QString& arg)
{
    const QString upper = cmd.toUpper();
    if (!isValidCommandToken(upper)) {
        return {};
    }
    return QStringLiteral("^%1%2;").arg(upper, arg).toLatin1();
}

void MessageParser::feed(const QByteArray& bytes)
{
    m_buf.append(bytes);

    int cursor = 0;
    // Next ';' at or after the current frame start, reused across iterations
    // so a run of '^' with no terminator is one scan, not one per '^'. -1
    // means there is no ';' anywhere ahead; -2 means not looked up yet.
    int end = -2;
    while (true) {
        const int start = m_buf.indexOf('^', cursor);
        if (start < 0) {
            // No frame start anywhere in what's left — it is all leading
            // garbage or a partial frame that never began. Drop it.
            cursor = m_buf.size();
            break;
        }
        if (end == -2 || (end >= 0 && end <= start)) {
            end = m_buf.indexOf(';', start + 1);
        }

        // A second frame START before this frame's terminator means the
        // '^' at `start` was garbage — a truncated frame, or a stray byte
        // that happened to be 0x5E. Resync onto the later '^' rather than
        // swallowing everything up to the next ';', which would eat the
        // genuine frame that follows.
        const int nextStart = m_buf.indexOf('^', start + 1);
        if (nextStart >= 0 && (end < 0 || nextStart < end)) {
            cursor = nextStart;
            continue;
        }

        if (end < 0) {
            // Incomplete frame: keep it for the next feed(), but only if it
            // is still a plausible length. A peer that opens a '^' and never
            // sends ';' must not be able to grow this buffer forever.
            if (m_buf.size() - start > kMaxArgChars + 8) {
                cursor = start + 1;  // resync past the bogus '^' and keep scanning
                continue;
            }
            cursor = start;
            break;
        }

        const QString body = QString::fromLatin1(m_buf.mid(start + 1, end - start - 1));
        QString cmd;
        QString arg;
        if (splitBody(body, cmd, arg) && m_onMessage) {
            m_onMessage(Message{cmd, arg});
        }
        cursor = end + 1;
    }

    m_buf = m_buf.mid(cursor);
    if (m_buf.size() > kMaxBufferBytes) {
        // Belt-and-braces cap. The resync above already bounds the normal
        // garbage case; this catches a stream that is nothing but '^'
        // characters, where every iteration legitimately keeps one byte.
        m_buf = m_buf.right(kMaxBufferBytes);
    }
}

std::optional<Message> parseMessage(const QByteArray& frame)
{
    const QByteArray trimmed = frame.trimmed();
    if (trimmed.size() < 4 || !trimmed.startsWith('^') || !trimmed.endsWith(';')) {
        return std::nullopt;
    }
    // Exactly ONE frame — a payload containing its own terminator is two
    // frames concatenated, which is MessageParser's job, not this one's.
    if (trimmed.indexOf(';') != trimmed.size() - 1) {
        return std::nullopt;
    }
    const QString body = QString::fromLatin1(trimmed.mid(1, trimmed.size() - 2));
    QString cmd;
    QString arg;
    if (!splitBody(body, cmd, arg)) {
        return std::nullopt;
    }
    return Message{cmd, arg};
}

QString atuModeLabel(AtuMode mode)
{
    switch (mode) {
        case AtuMode::Bypass: return QStringLiteral("BYPASS");
        case AtuMode::Inline: return QStringLiteral("INLINE");
        case AtuMode::Unknown: break;
    }
    return QStringLiteral("—");
}

bool applyMessage(const Message& message, Status& status)
{
    const QString& cmd = message.cmd;
    const QString& arg = message.arg;

    if (cmd == QLatin1String("PWF")) {
        return assignIfChanged(status.forwardWatts, decodeScaled(arg, kPowerDivisor));
    }
    if (cmd == QLatin1String("PWR")) {
        return assignIfChanged(status.reflectedWatts, decodeScaled(arg, kPowerDivisor));
    }
    if (cmd == QLatin1String("SW")) {
        return assignIfChanged(status.swr, decodeScaled(arg, kSwrDivisor));
    }
    if (cmd == QLatin1String("TM")) {
        return assignIfChanged(status.tempC, decodeInt(arg));
    }
    if (cmd == QLatin1String("BN")) {
        return assignIfChanged(status.band, decodeInt(arg));
    }
    if (cmd == QLatin1String("FL")) {
        return assignIfChanged(status.faultCode, decodeFault(arg));
    }
    if (cmd == QLatin1String("OS")) {
        return assignIfChanged(status.operate, decodeBool(arg));
    }
    if (cmd == QLatin1String("AN")) {
        const auto port = decodeInt(arg);
        if (!port || *port < kMinAntenna || *port > kMaxAntennaNumber) {
            return false;
        }
        return assignIfChanged(status.antenna, port);
    }
    if (cmd == QLatin1String("AM")) {
        // Only the current-band reply is a single letter; the per-band and
        // all-band forms carry band/antenna digits and are not polled.
        if (arg == QLatin1String("I")) {
            return assignIfChanged(status.atuMode, std::optional<AtuMode>(AtuMode::Inline));
        }
        if (arg == QLatin1String("B")) {
            return assignIfChanged(status.atuMode, std::optional<AtuMode>(AtuMode::Bypass));
        }
        return false;
    }
    if (cmd == QLatin1String("AI")) {
        return assignIfChanged(status.atuInline, decodeBool(arg));
    }
    if (cmd == QLatin1String("TP")) {
        return assignIfChanged(status.tuning, decodeBool(arg));
    }
    if (cmd == QLatin1String("TQ")) {
        return assignIfChanged(status.keyed, decodeKeyState(arg));
    }
    // Unrecognized command — deliberately not an error. The amp answers
    // several queries this integration does not send (and future firmware
    // will add more); dropping them keeps the parser forward-compatible
    // instead of desynchronizing on the first unknown token.
    return false;
}

QString bandName(int code)
{
    // The ^BN table from the Programming Reference (shared with the K3/K4).
    // Any code outside it renders as "—" rather than as a wrong band.
    switch (code) {
        case 0:  return QStringLiteral("160");
        case 1:  return QStringLiteral("80");
        case 2:  return QStringLiteral("60");
        case 3:  return QStringLiteral("40");
        case 4:  return QStringLiteral("30");
        case 5:  return QStringLiteral("20");
        case 6:  return QStringLiteral("17");
        case 7:  return QStringLiteral("15");
        case 8:  return QStringLiteral("12");
        case 9:  return QStringLiteral("10");
        case 10: return QStringLiteral("6");
        default: break;
    }
    return {};
}

QByteArray buildQuery(const QString& cmd)
{
    return buildMessage(cmd);
}

QByteArray buildSetOperate(bool operate)
{
    return buildMessage(QStringLiteral("OS"), operate ? QStringLiteral("1") : QStringLiteral("0"));
}

QByteArray buildSetAtuInline(bool inLine)
{
    return buildMessage(QStringLiteral("AI"), inLine ? QStringLiteral("1") : QStringLiteral("0"));
}

QByteArray buildStartTune()
{
    return buildMessage(QStringLiteral("FT"));
}

QByteArray buildCancelTune()
{
    return buildMessage(QStringLiteral("FE"));
}

QByteArray buildSelectAntenna(int port)
{
    if (port < kMinAntenna || port > kMaxAntenna) {
        return {};
    }
    return buildMessage(QStringLiteral("AN"), QString::number(port));
}

QByteArray buildClearFault()
{
    return buildMessage(QStringLiteral("FL"), QStringLiteral("C"));
}

QByteArray buildKey(int seconds)
{
    // Clamped, never rejected — see the header's doc comment. A caller that
    // asks for 0 or 500 seconds still gets a bounded, self-expiring key
    // rather than an empty frame (which would silently not key at all) or
    // an unbounded one (which would never self-release).
    const int bounded = std::clamp(seconds, kMinKeyTimeoutSec, kMaxKeyTimeoutSec);
    return buildMessage(QStringLiteral("TX"), QString::number(bounded));
}

QByteArray buildUnkey()
{
    return buildMessage(QStringLiteral("RX"));
}

QByteArray buildKeyStateQuery()
{
    return buildMessage(QStringLiteral("TQ"));
}

}  // namespace Kpa1500
}  // namespace AetherSDR
