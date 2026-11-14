#include "core/hfgateway/ModemHostProtocol.h"

#include <QRegularExpression>

#include <cmath>

namespace AetherSDR::ModemHost {

namespace {

constexpr char kCr = '\r';
constexpr char kLf = '\n';

bool printable(char c)
{
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x20 && u <= 0x7E;
}

QByteArray line(const QString& text)
{
    QByteArray out = text.toLatin1();
    out.append(kCr);
    return out;
}

QStringList tokens(const QString& text)
{
    return text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

// A decimal integer with no sign, spaces or trailing text.
bool parseUInt(const QString& s, qint64 max, qint64* out)
{
    if (s.isEmpty() || s.size() > 12) {
        return false;
    }
    for (const QChar c : s) {
        if (!c.isDigit()) {
            return false;
        }
    }
    bool ok = false;
    const qint64 v = s.toLongLong(&ok);
    if (!ok || v < 0 || v > max) {
        return false;
    }
    *out = v;
    return true;
}

// A plain decimal number, optionally signed, with at most one point.
bool parseDecimal(const QString& s, double* out)
{
    static const QRegularExpression re(QStringLiteral("^[+-]?[0-9]{1,4}(\\.[0-9]{1,3})?$"));
    if (!re.match(s).hasMatch()) {
        return false;
    }
    bool ok = false;
    const double v = s.toDouble(&ok);
    if (!ok || !std::isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

bool parseBandwidth(const QString& s, int* out)
{
    qint64 v = 0;
    if (!parseUInt(s, 10000, &v) || !isKnownBandwidth(static_cast<int>(v))) {
        return false;
    }
    *out = static_cast<int>(v);
    return true;
}

bool parseOnOff(const QString& s, bool* out)
{
    if (s == QLatin1String("ON")) {
        *out = true;
        return true;
    }
    if (s == QLatin1String("OFF")) {
        *out = false;
        return true;
    }
    return false;
}

Notification unknown(const QString& raw)
{
    Notification n;
    n.raw = raw;
    return n;
}

}  // namespace

// ── Framing ─────────────────────────────────────────────────────────────────

QList<QString> LineSplitter::feed(const QByteArray& bytes)
{
    QList<QString> out;
    for (const char c : bytes) {
        if (c == kLf) {
            continue;
        }
        if (c == kCr) {
            if (m_overflow || m_badByte) {
                ++m_rejected;
            } else if (!m_line.isEmpty()) {
                out.append(QString::fromLatin1(m_line));
            }
            m_line.clear();
            m_overflow = false;
            m_badByte = false;
            continue;
        }
        if (m_overflow) {
            continue;
        }
        if (!printable(c)) {
            m_badByte = true;
        }
        if (m_line.size() >= kMaxLineBytes) {
            m_overflow = true;
            m_line.clear();
            continue;
        }
        m_line.append(c);
    }
    return out;
}

void LineSplitter::reset()
{
    m_line.clear();
    m_overflow = false;
    m_badByte = false;
}

// ── Validation ──────────────────────────────────────────────────────────────

bool isValidCallsign(const QString& call)
{
    static const QRegularExpression re(
        QStringLiteral("^(?=[A-Z0-9]*[0-9])(?=[A-Z0-9]*[A-Z])[A-Z0-9]{1,7}(-(1[0-5]|[0-9]))?$"));
    return re.match(call).hasMatch();
}

bool isKnownBandwidth(int hz)
{
    return hz == 500 || hz == 1200 || hz == 2300 || hz == 2750;
}

// ── Modem -> host ───────────────────────────────────────────────────────────

Notification parseNotification(const QString& raw)
{
    const QStringList t = tokens(raw);
    if (t.isEmpty()) {
        return unknown(raw);
    }
    Notification n;
    n.raw = raw;
    const QString& head = t.first();

    if (t.size() == 1) {
        if (head == QLatin1String("OK")) {
            n.type = NotificationType::Ok;
        } else if (head == QLatin1String("WRONG")) {
            n.type = NotificationType::Wrong;
        } else if (head == QLatin1String("IAMALIVE")) {
            n.type = NotificationType::IAmAlive;
        } else if (head == QLatin1String("PENDING")) {
            n.type = NotificationType::Pending;
        } else if (head == QLatin1String("CANCELPENDING")) {
            n.type = NotificationType::CancelPending;
        } else if (head == QLatin1String("DISCONNECTED")) {
            n.type = NotificationType::Disconnected;
        } else {
            return unknown(raw);
        }
        return n;
    }

    if (head == QLatin1String("DISCONNECTED")) {
        // Data2G with DATA2G ON appends a reason ("DISCONNECTED ABORT").
        n.type = NotificationType::Disconnected;
        return n;
    }
    if ((head == QLatin1String("PTT") || head == QLatin1String("BUSY")) && t.size() == 2) {
        if (!parseOnOff(t.at(1), &n.on)) {
            return unknown(raw);
        }
        n.type = head == QLatin1String("PTT") ? NotificationType::Ptt : NotificationType::Busy;
        return n;
    }
    if (head == QLatin1String("CONNECTED") && t.size() == 4) {
        if (!isValidCallsign(t.at(1)) || !isValidCallsign(t.at(2))
            || !parseBandwidth(t.at(3), &n.bandwidthHz)) {
            return unknown(raw);
        }
        n.type = NotificationType::Connected;
        n.source = t.at(1);
        n.destination = t.at(2);
        return n;
    }
    if (head == QLatin1String("BUFFER") && t.size() == 2) {
        if (!parseUInt(t.at(1), kMaxBufferBytes, &n.bytes)) {
            return unknown(raw);
        }
        n.type = NotificationType::Buffer;
        return n;
    }
    if (head == QLatin1String("SN") && t.size() == 2) {
        if (!parseDecimal(t.at(1), &n.snDb) || n.snDb < kMinSnDb || n.snDb > kMaxSnDb) {
            return unknown(raw);
        }
        n.type = NotificationType::SignalToNoise;
        return n;
    }
    if (head == QLatin1String("BITRATE") && t.size() == 4 && t.at(3) == QLatin1String("BPS")) {
        // "BITRATE (<code>) <bps> BPS" (Mercury docs/TNC.md, BITRATE).
        const QString& code = t.at(1);
        qint64 c = 0;
        qint64 bps = 0;
        if (code.size() < 3 || !code.startsWith(QLatin1Char('(')) || !code.endsWith(QLatin1Char(')'))
            || !parseUInt(code.mid(1, code.size() - 2), 9999, &c) || !parseUInt(t.at(2), 1000000, &bps)) {
            return unknown(raw);
        }
        n.type = NotificationType::Bitrate;
        n.bitrateCode = static_cast<int>(c);
        n.bitsPerSecond = static_cast<int>(bps);
        return n;
    }
    if (head == QLatin1String("REGISTERED") && t.size() == 2) {
        if (!isValidCallsign(t.at(1))) {
            return unknown(raw);
        }
        n.type = NotificationType::Registered;
        n.callsign = t.at(1);
        return n;
    }
    if (head == QLatin1String("VERSION")) {
        n.type = NotificationType::Version;
        n.text = raw.mid(raw.indexOf(QLatin1String("VERSION")) + 7).trimmed();
        return n;
    }
    if (head == QLatin1String("CQFRAME") && t.size() == 3) {
        if (!isValidCallsign(t.at(1)) || !parseBandwidth(t.at(2), &n.bandwidthHz)) {
            return unknown(raw);
        }
        n.type = NotificationType::CqFrame;
        n.source = t.at(1);
        return n;
    }
    return unknown(raw);
}

QByteArray cmdMyCall(const QStringList& callsigns)
{
    if (callsigns.isEmpty() || callsigns.size() > kMaxMyCallCallsigns) {
        return {};
    }
    for (const QString& c : callsigns) {
        if (!isValidCallsign(c)) {
            return {};
        }
    }
    return line(QStringLiteral("MYCALL ") + callsigns.join(QLatin1Char(' ')));
}

QByteArray cmdListen(bool on)
{
    return line(on ? QStringLiteral("LISTEN ON") : QStringLiteral("LISTEN OFF"));
}

QByteArray cmdDisconnect() { return line(QStringLiteral("DISCONNECT")); }
QByteArray cmdAbort() { return line(QStringLiteral("ABORT")); }
QByteArray cmdVersion() { return line(QStringLiteral("VERSION")); }
QByteArray cmdBuffer() { return line(QStringLiteral("BUFFER")); }
QByteArray cmdSn() { return line(QStringLiteral("SN")); }

// ── Host -> modem ───────────────────────────────────────────────────────────

Command parseCommand(const QString& raw)
{
    Command c;
    c.raw = raw;
    const QStringList t = tokens(raw);
    if (t.isEmpty()) {
        return c;
    }
    const QString& head = t.first();
    c.args = t.mid(1);
    const int argc = static_cast<int>(c.args.size());

    const auto invalid = [&c]() {
        c.type = CommandType::Invalid;
        return c;
    };

    if (head == QLatin1String("MYCALL")) {
        if (argc < 1 || argc > kMaxMyCallCallsigns) {
            return invalid();
        }
        for (const QString& call : c.args) {
            if (!isValidCallsign(call)) {
                return invalid();
            }
        }
        c.type = CommandType::MyCall;
        c.callsigns = c.args;
        return c;
    }
    if (head == QLatin1String("LISTEN")) {
        if (argc != 1) {
            return invalid();
        }
        if (c.args.first() == QLatin1String("ON")) {
            c.type = CommandType::ListenOn;
        } else if (c.args.first() == QLatin1String("OFF")) {
            c.type = CommandType::ListenOff;
        } else if (c.args.first() == QLatin1String("CQ")) {
            c.type = CommandType::ListenCq;
        } else {
            return invalid();
        }
        return c;
    }
    if (head == QLatin1String("CONNECT")) {
        if (argc != 2 || !isValidCallsign(c.args.at(0)) || !isValidCallsign(c.args.at(1))) {
            return invalid();
        }
        c.type = CommandType::Connect;
        c.source = c.args.at(0);
        c.destination = c.args.at(1);
        return c;
    }
    if (head == QLatin1String("CQFRAME")) {
        if (argc != 2 || !isValidCallsign(c.args.at(0)) || !parseBandwidth(c.args.at(1), &c.bandwidthHz)) {
            return invalid();
        }
        c.type = CommandType::CqFrame;
        c.source = c.args.at(0);
        return c;
    }
    if (head.startsWith(QLatin1String("BW")) && argc == 0) {
        if (!parseBandwidth(head.mid(2), &c.bandwidthHz)) {
            return invalid();
        }
        c.type = CommandType::Bandwidth;
        return c;
    }

    struct Simple {
        const char* word;
        CommandType type;
    };
    // Commands whose arguments the gateway does not act on: matched by their
    // first token so they can be answered (usually WRONG or OK) and logged.
    static constexpr Simple kSimple[] = {
        {"DISCONNECT", CommandType::Disconnect},
        {"ABORT", CommandType::Abort},
        {"VERSION", CommandType::Version},
        {"BUFFER", CommandType::Buffer},
        {"SN", CommandType::Sn},
        {"BITRATE", CommandType::Bitrate},
        {"TUNE", CommandType::Tune},
        {"ENCRYPTION", CommandType::Encryption},
        {"CHAT", CommandType::Chat},
        {"PUBLIC", CommandType::Public},
        {"COMPRESSION", CommandType::Compression},
        {"CLEANTXBUFFER", CommandType::CleanTxBuffer},
    };
    for (const Simple& s : kSimple) {
        if (head == QLatin1String(s.word)) {
            c.type = s.type;
            return c;
        }
    }
    return c;
}

QByteArray fmtOk() { return line(QStringLiteral("OK")); }
QByteArray fmtWrong() { return line(QStringLiteral("WRONG")); }
QByteArray fmtIAmAlive() { return line(QStringLiteral("IAMALIVE")); }
QByteArray fmtPending() { return line(QStringLiteral("PENDING")); }
QByteArray fmtCancelPending() { return line(QStringLiteral("CANCELPENDING")); }
QByteArray fmtDisconnected() { return line(QStringLiteral("DISCONNECTED")); }
QByteArray fmtPtt(bool on) { return line(on ? QStringLiteral("PTT ON") : QStringLiteral("PTT OFF")); }
QByteArray fmtBusy(bool on) { return line(on ? QStringLiteral("BUSY ON") : QStringLiteral("BUSY OFF")); }

QByteArray fmtConnected(const QString& source, const QString& destination, int bandwidthHz)
{
    if (!isValidCallsign(source) || !isValidCallsign(destination) || !isKnownBandwidth(bandwidthHz)) {
        return {};
    }
    return line(QStringLiteral("CONNECTED %1 %2 %3").arg(source, destination).arg(bandwidthHz));
}

QByteArray fmtBuffer(qint64 bytes)
{
    if (bytes < 0 || bytes > kMaxBufferBytes) {
        return {};
    }
    return line(QStringLiteral("BUFFER %1").arg(bytes));
}

QByteArray fmtSn(double snDb)
{
    if (!std::isfinite(snDb) || snDb < kMinSnDb || snDb > kMaxSnDb) {
        return {};
    }
    return line(QStringLiteral("SN %1").arg(snDb, 0, 'f', 1));
}

QByteArray fmtVersion(const QString& text)
{
    QString clean;
    for (const QChar ch : text) {
        if (ch.unicode() >= 0x20 && ch.unicode() <= 0x7E) {
            clean.append(ch);
        }
    }
    clean = clean.left(64).trimmed();
    if (clean.isEmpty()) {
        return {};
    }
    return line(QStringLiteral("VERSION ") + clean);
}

QString toString(NotificationType type)
{
    switch (type) {
    case NotificationType::Unknown: return QStringLiteral("Unknown");
    case NotificationType::Ok: return QStringLiteral("Ok");
    case NotificationType::Wrong: return QStringLiteral("Wrong");
    case NotificationType::IAmAlive: return QStringLiteral("IAmAlive");
    case NotificationType::Pending: return QStringLiteral("Pending");
    case NotificationType::CancelPending: return QStringLiteral("CancelPending");
    case NotificationType::Connected: return QStringLiteral("Connected");
    case NotificationType::Disconnected: return QStringLiteral("Disconnected");
    case NotificationType::Ptt: return QStringLiteral("Ptt");
    case NotificationType::Busy: return QStringLiteral("Busy");
    case NotificationType::Buffer: return QStringLiteral("Buffer");
    case NotificationType::SignalToNoise: return QStringLiteral("SignalToNoise");
    case NotificationType::Bitrate: return QStringLiteral("Bitrate");
    case NotificationType::Registered: return QStringLiteral("Registered");
    case NotificationType::Version: return QStringLiteral("Version");
    case NotificationType::CqFrame: return QStringLiteral("CqFrame");
    }
    return QStringLiteral("Unknown");
}

QString toString(CommandType type)
{
    switch (type) {
    case CommandType::Unknown: return QStringLiteral("Unknown");
    case CommandType::Invalid: return QStringLiteral("Invalid");
    case CommandType::MyCall: return QStringLiteral("MyCall");
    case CommandType::ListenOn: return QStringLiteral("ListenOn");
    case CommandType::ListenOff: return QStringLiteral("ListenOff");
    case CommandType::ListenCq: return QStringLiteral("ListenCq");
    case CommandType::Connect: return QStringLiteral("Connect");
    case CommandType::Disconnect: return QStringLiteral("Disconnect");
    case CommandType::Abort: return QStringLiteral("Abort");
    case CommandType::Version: return QStringLiteral("Version");
    case CommandType::Buffer: return QStringLiteral("Buffer");
    case CommandType::Sn: return QStringLiteral("Sn");
    case CommandType::Bitrate: return QStringLiteral("Bitrate");
    case CommandType::Bandwidth: return QStringLiteral("Bandwidth");
    case CommandType::CqFrame: return QStringLiteral("CqFrame");
    case CommandType::Tune: return QStringLiteral("Tune");
    case CommandType::Encryption: return QStringLiteral("Encryption");
    case CommandType::Chat: return QStringLiteral("Chat");
    case CommandType::Public: return QStringLiteral("Public");
    case CommandType::Compression: return QStringLiteral("Compression");
    case CommandType::CleanTxBuffer: return QStringLiteral("CleanTxBuffer");
    }
    return QStringLiteral("Unknown");
}

}  // namespace AetherSDR::ModemHost
