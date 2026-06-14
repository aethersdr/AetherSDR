#include "core/aprs/AprsPacket.h"

#include <QRegularExpression>
#include <QtMath>

namespace AetherSDR::aprs {

namespace {

// --- Uncompressed position: "4903.50N/07201.75W-" ------------------------

// Parses "ddmm.mmN" / "dddmm.mmW". Position ambiguity (trailing spaces in
// the digits, APRS 1.0.1 ch. 6) is resolved to the cell center by replacing
// spaces with '5' in the first blanked digit and '0' after.
std::optional<double> parseLatLon(QString text, bool isLongitude)
{
    const int degDigits = isLongitude ? 3 : 2;
    if (text.size() != degDigits + 6)
        return std::nullopt;
    const QChar hemi = text.back().toUpper();
    text.chop(1);
    bool firstBlank = true;
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char(' ')) {
            text[i] = firstBlank ? QLatin1Char('5') : QLatin1Char('0');
            firstBlank = false;
        }
    }
    bool degOk = false, minOk = false;
    const double deg = text.left(degDigits).toDouble(&degOk);
    const double min = text.mid(degDigits).toDouble(&minOk);
    if (!degOk || !minOk || min >= 60.0)
        return std::nullopt;
    double value = deg + min / 60.0;
    if (isLongitude) {
        if (value > 180.0)
            return std::nullopt;
        if (hemi == QLatin1Char('W'))
            value = -value;
        else if (hemi != QLatin1Char('E'))
            return std::nullopt;
    } else {
        if (value > 90.0)
            return std::nullopt;
        if (hemi == QLatin1Char('S'))
            value = -value;
        else if (hemi != QLatin1Char('N'))
            return std::nullopt;
    }
    return value;
}

// Course/speed data extension "088/036" at the start of the comment
// (APRS 1.0.1 ch. 7). Consumes the 7 characters when present.
void extractCourseSpeed(QString& comment, Packet& pkt)
{
    static const QRegularExpression re(
        QStringLiteral("^([0-9 .]{3})/([0-9 .]{3})"));
    const QRegularExpressionMatch m = re.match(comment);
    if (!m.hasMatch())
        return;
    bool crsOk = false, spdOk = false;
    const int crs = m.captured(1).trimmed().toInt(&crsOk);
    const int spd = m.captured(2).trimmed().toInt(&spdOk);
    if (crsOk && crs >= 1 && crs <= 360)
        pkt.courseDeg = (crs == 360) ? 0.0 : double(crs);
    if (spdOk)
        pkt.speedKnots = double(spd);
    if (crsOk || spdOk)
        comment.remove(0, 7);
}

// Altitude comment extension "/A=001234" (feet), anywhere in the comment.
void extractAltitude(QString& comment, Packet& pkt)
{
    static const QRegularExpression re(QStringLiteral("/A=(-?\\d{6})"));
    const QRegularExpressionMatch m = re.match(comment);
    if (!m.hasMatch())
        return;
    pkt.hasAltitude = true;
    pkt.altitudeFeet = m.captured(1).toDouble();
    comment.remove(m.capturedStart(0), m.capturedLength(0));
}

bool isValidSymbolTable(QChar c)
{
    // '/', '\', or an overlay 0-9 A-Z (compressed also uses a-j for 0-9).
    return c == QLatin1Char('/') || c == QLatin1Char('\\')
        || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
        || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
        || (c >= QLatin1Char('a') && c <= QLatin1Char('j'));
}

// Compressed position "/5L!!<*e7>7P[" (APRS 1.0.1 ch. 9): symbol table,
// 4-char base-91 lat, 4-char base-91 lon, symbol code, cs, comp-type.
// Returns the number of characters consumed (13), or 0 on failure.
int parseCompressedPosition(const QString& body, Packet& pkt)
{
    if (body.size() < 13 || !isValidSymbolTable(body.at(0)))
        return 0;
    auto base91 = [](const QString& s, double& out) {
        double v = 0;
        for (const QChar c : s) {
            const int d = c.toLatin1() - 33;
            if (d < 0 || d > 90)
                return false;
            v = v * 91.0 + d;
        }
        out = v;
        return true;
    };
    double latVal = 0, lonVal = 0;
    if (!base91(body.mid(1, 4), latVal) || !base91(body.mid(5, 4), lonVal))
        return 0;
    pkt.hasPosition = true;
    pkt.latitude = 90.0 - latVal / 380926.0;
    pkt.longitude = -180.0 + lonVal / 190463.0;
    // Overlay digits are transmitted a-j in compressed form; normalize to 0-9.
    const QChar table = body.at(0);
    pkt.symbolTable = (table >= QLatin1Char('a') && table <= QLatin1Char('j'))
        ? char('0' + (table.toLatin1() - 'a'))
        : table.toLatin1();
    pkt.symbolCode = body.at(9).toLatin1();

    const char c = body.at(10).toLatin1();
    const char s = body.at(11).toLatin1();
    const char compType = body.at(12).toLatin1();
    if (c != ' ' && c >= '!' && c <= 'z' && compType >= '!') {
        const int nmeaBits = ((compType - 33) >> 3) & 0x03;
        if (nmeaBits == 2) { // cs carries altitude: 1.002^(c*91+s) feet
            pkt.hasAltitude = true;
            pkt.altitudeFeet = qPow(1.002, double((c - 33) * 91 + (s - 33)));
        } else if (c <= 'z' && s >= '!') {
            pkt.courseDeg = double((c - 33) * 4);
            pkt.speedKnots = qPow(1.08, double(s - 33)) - 1.0;
        }
    }
    return 13;
}

// Uncompressed position at the head of `body`; returns chars consumed or 0.
int parseUncompressedPosition(const QString& body, Packet& pkt)
{
    if (body.size() < 19)
        return 0;
    const auto lat = parseLatLon(body.left(8), false);
    const auto lon = parseLatLon(body.mid(9, 9), true);
    if (!lat || !lon || !isValidSymbolTable(body.at(8)))
        return 0;
    pkt.hasPosition = true;
    pkt.latitude = *lat;
    pkt.longitude = *lon;
    pkt.symbolTable = body.at(8).toLatin1();
    pkt.symbolCode = body.at(18).toLatin1();
    return 19;
}

// Either position form at the head of `body`; comment handling shared.
void parsePositionBody(QString body, Packet& pkt)
{
    int used = 0;
    if (!body.isEmpty() && body.at(0).isDigit())
        used = parseUncompressedPosition(body, pkt);
    else
        used = parseCompressedPosition(body, pkt);
    if (!used)
        return;
    QString comment = body.mid(used);
    if (pkt.courseDeg < 0 && pkt.speedKnots < 0)
        extractCourseSpeed(comment, pkt);
    extractAltitude(comment, pkt);
    pkt.comment = comment.trimmed();
}

// "123456z" / "123456h" / "123456/" timestamp prefix (ch. 6); we only need
// to skip it — the decode timestamp is when *we* heard the frame.
int timestampLength(const QString& body)
{
    if (body.size() < 7)
        return 0;
    for (int i = 0; i < 6; ++i) {
        if (!body.at(i).isDigit())
            return 0;
    }
    const QChar suffix = body.at(6).toLower();
    if (suffix == QLatin1Char('z') || suffix == QLatin1Char('h')
        || suffix == QLatin1Char('/'))
        return 7;
    return 0;
}

// --- Mic-E (APRS 1.0.1 ch. 10) -------------------------------------------

// Decode one Mic-E destination-field character into its latitude digit.
// Returns -1 for invalid, 10 for "space" (position ambiguity).
int micEDigit(QChar c)
{
    const char ch = c.toLatin1();
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'A' && ch <= 'J')
        return ch - 'A';
    if (ch == 'K' || ch == 'L' || ch == 'Z')
        return 10; // space (ambiguity)
    if (ch >= 'P' && ch <= 'Y')
        return ch - 'P';
    return -1;
}

bool parseMicE(const ax25::Frame& frame, const QString& info, Packet& pkt)
{
    // The latitude rides in the 6-character destination callsign field.
    const QString dest = frame.dest.call.toUpper();
    if (dest.size() != 6 || info.size() < 9)
        return false;

    int digits[6];
    for (int i = 0; i < 6; ++i) {
        digits[i] = micEDigit(dest.at(i));
        if (digits[i] < 0)
            return false;
    }
    // Ambiguity digits read as the cell center, matching parseLatLon().
    auto digitOr = [&](int i, int center) {
        return digits[i] == 10 ? center : digits[i];
    };
    const double latDeg = digitOr(0, 0) * 10 + digitOr(1, 0);
    const double latMin = digitOr(2, 5) * 10 + digitOr(3, 0)
        + (digitOr(4, 0) * 10 + digitOr(5, 0)) / 100.0;
    if (latDeg > 90.0 || latMin >= 60.0)
        return false;
    double lat = latDeg + latMin / 60.0;

    // Flag bits live in the character *ranges* of dest chars 3-5.
    auto isOneBit = [&](int i) {
        const char ch = dest.at(i).toLatin1();
        return (ch >= 'P' && ch <= 'Z');
    };
    const bool north = isOneBit(3);
    const bool lonOffset = isOneBit(4);
    const bool west = isOneBit(5);
    if (!north)
        lat = -lat;

    // Longitude: info bytes 1-3 (d+28, m+28, h+28).
    const int d28 = quint8(info.at(1).toLatin1());
    const int m28 = quint8(info.at(2).toLatin1());
    const int h28 = quint8(info.at(3).toLatin1());
    int lonDeg = d28 - 28;
    if (lonOffset)
        lonDeg += 100;
    if (lonDeg >= 180 && lonDeg <= 189)
        lonDeg -= 80;
    else if (lonDeg >= 190 && lonDeg <= 199)
        lonDeg -= 190;
    int lonMin = m28 - 28;
    if (lonMin >= 60)
        lonMin -= 60;
    const int lonHun = h28 - 28;
    if (lonDeg > 180 || lonMin > 59 || lonHun < 0 || lonHun > 99)
        return false;
    double lon = lonDeg + (lonMin + lonHun / 100.0) / 60.0;
    if (west)
        lon = -lon;

    // Speed/course: info bytes 4-6 (SP+28, DC+28, SE+28).
    const int sp = quint8(info.at(4).toLatin1()) - 28;
    const int dc = quint8(info.at(5).toLatin1()) - 28;
    const int se = quint8(info.at(6).toLatin1()) - 28;
    if (sp < 0 || dc < 0 || se < 0)
        return false;
    int speed = sp * 10 + dc / 10;
    int course = (dc % 10) * 100 + se;
    if (speed >= 800)
        speed -= 800;
    if (course >= 400)
        course -= 400;

    pkt.type = PacketType::Position;
    pkt.hasPosition = true;
    pkt.latitude = lat;
    pkt.longitude = lon;
    pkt.symbolCode = info.at(7).toLatin1();
    pkt.symbolTable = info.size() > 8 ? info.at(8).toLatin1() : '/';
    pkt.speedKnots = double(speed);
    if (course >= 1 && course <= 360)
        pkt.courseDeg = (course == 360) ? 0.0 : double(course);
    pkt.messagingCapable = true; // Mic-E rigs are message-capable by design

    QString comment = info.mid(9);
    // Optional Mic-E telemetry/device prefix and "xxx}" base-91 altitude.
    if (!comment.isEmpty()
        && (comment.at(0) == QLatin1Char('>') || comment.at(0) == QLatin1Char(']')
            || comment.at(0) == QLatin1Char('`') || comment.at(0) == QLatin1Char('\'')))
        comment.remove(0, 1);
    if (comment.size() >= 4 && comment.at(3) == QLatin1Char('}')) {
        int alt = 0;
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            const int d = comment.at(i).toLatin1() - 33;
            if (d < 0 || d > 90) {
                ok = false;
                break;
            }
            alt = alt * 91 + d;
        }
        if (ok) {
            pkt.hasAltitude = true;
            pkt.altitudeFeet = double(alt - 10000) * 3.28084; // meters → feet
            comment.remove(0, 4);
        }
    }
    pkt.comment = comment.trimmed();
    return true;
}

// --- Weather (ch. 12) ------------------------------------------------------

// Consume the concatenated token stream "g012t077r000p000P000h50b09900..."
// from the head of `text`. Each token is a tag letter plus a fixed-width
// field; dots or spaces in the field mean "sensor not present". Stops at the
// first unrecognized tag (the remainder is the station's free text).
void parseWeatherTokens(QString& text, WeatherReport& wx)
{
    static const QRegularExpression fieldChars(QStringLiteral("^[-0-9. ]+$"));
    while (!text.isEmpty()) {
        int len = 0;
        const char tag = text.at(0).toLatin1();
        switch (tag) {
        case 'g': case 't': case 'r': case 'p': case 'P': case 's':
            len = 3; break;       // s = snow (consumed, not surfaced)
        case 'h':
            len = 2; break;
        case 'b':
            len = 5; break;
        case 'L': case 'l':
            len = 3; break;       // luminosity (consumed, not surfaced)
        default:
            len = 0; break;
        }
        if (len == 0 || text.size() < 1 + len)
            return;
        const QString field = text.mid(1, len);
        if (!fieldChars.match(field).hasMatch())
            return;
        text.remove(0, 1 + len);
        bool ok = false;
        const double v = field.trimmed().toDouble(&ok);
        if (!ok)
            continue; // "..." — sensor not present
        switch (tag) {
        case 'g': wx.gustMph = v; wx.valid = true; break;
        case 't': wx.temperatureF = v; wx.valid = true; break;
        case 'r': wx.rainHourIn = v / 100.0; wx.valid = true; break;
        case 'p': wx.rain24hIn = v / 100.0; wx.valid = true; break;
        case 'P': wx.rainMidnightIn = v / 100.0; wx.valid = true; break;
        case 'h':
            wx.humidityPct = (int(v) == 0) ? 100 : int(v); // h00 = 100%
            wx.valid = true;
            break;
        case 'b': wx.pressureMbar = v / 10.0; wx.valid = true; break;
        default: break;
        }
    }
}

// Fold the wx data riding in a *position* weather report (symbol '_'): wind
// comes from the CSE/SPD extension (knots), the rest from the comment's
// token stream. The comment is replaced with the readable summary.
void extractPositionWeather(Packet& pkt)
{
    if (pkt.courseDeg >= 0.0)
        pkt.weather.windDirDeg = qRound(pkt.courseDeg);
    if (pkt.speedKnots >= 0.0)
        pkt.weather.windMph = pkt.speedKnots * 1.15078;
    pkt.weather.valid =
        (pkt.weather.windDirDeg >= 0) || (pkt.weather.windMph >= 0);
    QString rest = pkt.comment;
    parseWeatherTokens(rest, pkt.weather);
    if (!pkt.weather.valid)
        return;
    // Wind in the extension was wind, not movement — don't show a weather
    // station "driving" at 5 mph in the station table.
    pkt.courseDeg = -1.0;
    pkt.speedKnots = -1.0;
    QString comment = weatherSummary(pkt.weather);
    rest = rest.trimmed();
    if (!rest.isEmpty())
        comment += QStringLiteral("   ") + rest;
    pkt.comment = comment;
}

// --- Messages: ":ADDRESSEE:text{NN" (ch. 14) ------------------------------

bool parseMessage(const QString& body, Packet& pkt)
{
    // body excludes the ':' DTI. Addressee field is exactly 9 chars + ':'.
    if (body.size() < 10 || body.at(9) != QLatin1Char(':'))
        return false;
    pkt.addressee = body.left(9).trimmed().toUpper();
    if (pkt.addressee.isEmpty())
        return false;
    QString text = body.mid(10);

    static const QRegularExpression ackRej(
        QStringLiteral("^(ack|rej)([A-Za-z0-9]{1,5})$"));
    const QRegularExpressionMatch m = ackRej.match(text);
    if (m.hasMatch()) {
        pkt.type = (m.captured(1) == QLatin1String("ack"))
            ? PacketType::MessageAck : PacketType::MessageRej;
        pkt.messageNo = m.captured(2);
        return true;
    }

    const int brace = text.lastIndexOf(QLatin1Char('{'));
    if (brace >= 0 && text.size() - brace - 1 >= 1 && text.size() - brace - 1 <= 5) {
        pkt.messageNo = text.mid(brace + 1);
        text.truncate(brace);
    }
    pkt.type = PacketType::Message;
    pkt.messageText = text;
    return true;
}

} // namespace

std::optional<Packet> parseFrame(const ax25::Frame& frame)
{
    if (frame.type != ax25::FrameType::UI || frame.info.isEmpty())
        return std::nullopt;

    Packet pkt;
    pkt.source = frame.src.toString();
    pkt.destination = frame.dest.toString();
    for (const ax25::Address& hop : frame.via)
        pkt.path << hop.toString()
            + (hop.hasBeenRepeated ? QStringLiteral("*") : QString());

    const QString info = QString::fromLatin1(frame.info);
    pkt.infoText = info;
    const char dti = frame.info.at(0);

    switch (dti) {
    case '!':
    case '=':
        pkt.type = PacketType::Position;
        pkt.messagingCapable = (dti == '=');
        parsePositionBody(info.mid(1), pkt);
        if (pkt.hasPosition && pkt.symbolCode == '_') {
            pkt.type = PacketType::Weather;
            extractPositionWeather(pkt);
        }
        break;
    case '/':
    case '@': {
        pkt.type = PacketType::Position;
        pkt.messagingCapable = (dti == '@');
        QString body = info.mid(1);
        body.remove(0, timestampLength(body));
        parsePositionBody(body, pkt);
        if (pkt.hasPosition && pkt.symbolCode == '_') {
            pkt.type = PacketType::Weather;
            extractPositionWeather(pkt);
        }
        break;
    }
    case 0x1c: // Mic-E (current, rev 0)
    case 0x1d: // Mic-E (old, rev 0)
    case '`':  // Mic-E (current)
    case '\'': // Mic-E (old) — also pre-2000 TNC position; Mic-E wins here
        if (!parseMicE(frame, info, pkt))
            pkt.type = PacketType::Other;
        break;
    case '>':
        pkt.type = PacketType::Status;
        pkt.comment = info.mid(1 + timestampLength(info.mid(1))).trimmed();
        break;
    case ':':
        if (!parseMessage(info.mid(1), pkt))
            pkt.type = PacketType::Other;
        break;
    case ';': {
        // ";NAME *DDHHMMz<position>" — alive marker '*' or killed '_'.
        if (info.size() < 19) {
            pkt.type = PacketType::Other;
            break;
        }
        pkt.type = PacketType::Object;
        pkt.objectName = info.mid(1, 9).trimmed();
        QString body = info.mid(11);
        body.remove(0, timestampLength(body));
        parsePositionBody(body, pkt);
        break;
    }
    case ')': {
        // ")NAME!<position>" — name is 3-9 chars ended by '!' (live) / '_'.
        const QString body = info.mid(1);
        int sep = -1;
        for (int i = 0; i < body.size() && i < 10; ++i) {
            if (body.at(i) == QLatin1Char('!') || body.at(i) == QLatin1Char('_')) {
                sep = i;
                break;
            }
        }
        if (sep < 3) {
            pkt.type = PacketType::Other;
            break;
        }
        pkt.type = PacketType::Item;
        pkt.objectName = body.left(sep).trimmed();
        parsePositionBody(body.mid(sep + 1), pkt);
        break;
    }
    case '_': {
        // Positionless weather: "_MMDDHHMM" timestamp, "cDDDsSSS" wind
        // (degrees + mph), then the shared token stream.
        pkt.type = PacketType::Weather;
        QString body = info.mid(1);
        int digits = 0;
        while (digits < 8 && digits < body.size() && body.at(digits).isDigit())
            ++digits;
        if (digits == 8)
            body.remove(0, 8);
        if (body.size() >= 8 && body.at(0) == QLatin1Char('c')
            && body.at(4) == QLatin1Char('s')) {
            bool dirOk = false, spdOk = false;
            const int dir = body.mid(1, 3).toInt(&dirOk);
            const double spd = body.mid(5, 3).trimmed().toDouble(&spdOk);
            if (dirOk && dir >= 0 && dir <= 360) {
                pkt.weather.windDirDeg = (dir == 360) ? 0 : dir;
                pkt.weather.valid = true;
            }
            if (spdOk) {
                pkt.weather.windMph = spd;
                pkt.weather.valid = true;
            }
            body.remove(0, 8);
        }
        parseWeatherTokens(body, pkt.weather);
        if (pkt.weather.valid) {
            QString comment = weatherSummary(pkt.weather);
            body = body.trimmed();
            // Whatever trails the tokens is the station-type/software code
            // ("wRSW") — not worth a column's width.
            if (body.size() > 4)
                comment += QStringLiteral("   ") + body;
            pkt.comment = comment;
        } else {
            pkt.comment = info.mid(1).trimmed();
        }
        break;
    }
    case 'T':
        pkt.type = PacketType::Telemetry;
        pkt.comment = info.mid(1).trimmed();
        break;
    case '?':
        pkt.type = PacketType::Query;
        pkt.comment = info.mid(1).trimmed();
        break;
    default:
        pkt.type = PacketType::Other;
        pkt.comment = info.trimmed();
        break;
    }
    return pkt;
}

QString encodeUncompressedPosition(double lat, double lon,
                                   char symbolTable, char symbolCode,
                                   const QString& comment,
                                   bool messagingCapable)
{
    lat = qBound(-90.0, lat, 90.0);
    lon = qBound(-180.0, lon, 180.0);
    const char latHemi = lat < 0 ? 'S' : 'N';
    const char lonHemi = lon < 0 ? 'W' : 'E';
    lat = qAbs(lat);
    lon = qAbs(lon);
    int latDeg = int(lat);
    int lonDeg = int(lon);
    // Round minutes to the 2 decimals we print, then carry a 60.00 overflow
    // into the degrees. Without this, e.g. 38.999999° formats latMin=59.99994
    // as "60.00" → "3860.00N", which every APRS parser (including ours)
    // rejects as min >= 60. A GPS-fed beacon hits this near minute boundaries.
    double latMin = qRound((lat - latDeg) * 60.0 * 100.0) / 100.0;
    double lonMin = qRound((lon - lonDeg) * 60.0 * 100.0) / 100.0;
    if (latMin >= 60.0) { latMin -= 60.0; latDeg += 1; }
    if (lonMin >= 60.0) { lonMin -= 60.0; lonDeg += 1; }
    QString out;
    out += QLatin1Char(messagingCapable ? '=' : '!');
    out += QString::asprintf("%02d%05.2f%c", latDeg, latMin, latHemi);
    out += QLatin1Char(symbolTable);
    out += QString::asprintf("%03d%05.2f%c", lonDeg, lonMin, lonHemi);
    out += QLatin1Char(symbolCode);
    out += comment.trimmed();
    return out;
}

QString encodeMessage(const QString& addressee, const QString& text,
                      const QString& msgNo)
{
    QString out = QStringLiteral(":%1:%2")
        .arg(addressee.trimmed().toUpper().left(9), -9, QLatin1Char(' '))
        .arg(text);
    if (!msgNo.isEmpty())
        out += QLatin1Char('{') + msgNo.left(5);
    return out;
}

QString encodeAck(const QString& addressee, const QString& msgNo)
{
    return QStringLiteral(":%1:ack%2")
        .arg(addressee.trimmed().toUpper().left(9), -9, QLatin1Char(' '))
        .arg(msgNo.left(5));
}

QString encodeStatus(const QString& text)
{
    return QLatin1Char('>') + text.trimmed();
}

QString gridSquare(double lat, double lon)
{
    lat = qBound(-90.0, lat, 89.99999) + 90.0;
    lon = qBound(-180.0, lon, 179.99999) + 180.0;
    QString out;
    out += QChar('A' + int(lon / 20.0));
    out += QChar('A' + int(lat / 10.0));
    out += QChar('0' + int(fmod(lon, 20.0) / 2.0));
    out += QChar('0' + int(fmod(lat, 10.0)));
    out += QChar('a' + int(fmod(lon, 2.0) * 12.0));
    out += QChar('a' + int(fmod(lat, 1.0) * 24.0));
    return out;
}

double distanceMiles(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double kEarthRadiusMiles = 3958.756;
    const double p1 = qDegreesToRadians(lat1);
    const double p2 = qDegreesToRadians(lat2);
    const double dp = qDegreesToRadians(lat2 - lat1);
    const double dl = qDegreesToRadians(lon2 - lon1);
    const double a = qSin(dp / 2) * qSin(dp / 2)
        + qCos(p1) * qCos(p2) * qSin(dl / 2) * qSin(dl / 2);
    return kEarthRadiusMiles * 2.0 * qAtan2(qSqrt(a), qSqrt(1.0 - a));
}

double bearingDeg(double lat1, double lon1, double lat2, double lon2)
{
    const double p1 = qDegreesToRadians(lat1);
    const double p2 = qDegreesToRadians(lat2);
    const double dl = qDegreesToRadians(lon2 - lon1);
    const double y = qSin(dl) * qCos(p2);
    const double x = qCos(p1) * qSin(p2) - qSin(p1) * qCos(p2) * qCos(dl);
    const double deg = qRadiansToDegrees(qAtan2(y, x));
    return fmod(deg + 360.0, 360.0);
}

QString symbolDescription(char symbolTable, char symbolCode)
{
    struct Entry { char code; const char* primary; const char* alternate; };
    // The handful of symbols that actually dominate on-air traffic
    // (APRS 1.0.1 appendix 2). Everything else falls through generically.
    static constexpr Entry kSymbols[] = {
        { '!', "Police", "Emergency" },
        { '#', "Digipeater", "Digipeater (overlay)" },
        { '$', "Phone", "Bank" },
        { '\'', "Small aircraft", "Crash site" },
        { '-', "Home", "Home (HF)" },
        { '<', "Motorcycle", "Advisory" },
        { '=', "Railroad engine", "Railroad" },
        { '>', "Car", "Car (overlay)" },
        { 'O', "Balloon", "Rocket" },
        { 'P', "Police", "Parking" },
        { 'R', "RV", "Restaurant" },
        { 'U', "Bus", "Sunny" },
        { 'W', "Weather service", "Flooding" },
        { 'X', "Helicopter", "Pharmacy" },
        { 'Y', "Sailboat", "Radios/devices" },
        { '[', "Jogger", "Wall cloud" },
        { '_', "Weather station", "Weather (overlay)" },
        { 'a', "Ambulance", "ARRL/ARES" },
        { 'b', "Bicycle", "Blowing dust" },
        { 'f', "Fire truck", "Fog" },
        { 'g', "Glider", "Gale flags" },
        { 'j', "Jeep", "Lightning" },
        { 'k', "Truck", "Kenwood HT" },
        { 'm', "Mic-E repeater", "Milepost" },
        { 'r', "Repeater", "Restrooms" },
        { 's', "Power boat", "Ship/boat" },
        { 'u', "Truck (18-wheeler)", "Truck stop" },
        { 'v', "Van", "Van (overlay)" },
        { 'y', "Yagi at QTH", "Skywarn" },
    };
    const bool primary = (symbolTable == '/');
    for (const Entry& e : kSymbols) {
        if (e.code == symbolCode)
            return QString::fromLatin1(primary ? e.primary : e.alternate);
    }
    return QStringLiteral("Symbol '%1%2'")
        .arg(QLatin1Char(symbolTable)).arg(QLatin1Char(symbolCode));
}

QString weatherSummary(const WeatherReport& wx)
{
    QStringList parts;
    if (wx.temperatureF > -999.0)
        parts << QStringLiteral("%1°F").arg(qRound(wx.temperatureF));
    if (wx.windDirDeg >= 0 || wx.windMph >= 0.0) {
        QString wind = QStringLiteral("Wind");
        if (wx.windDirDeg >= 0)
            wind += QStringLiteral(" %1°").arg(wx.windDirDeg);
        if (wx.windMph >= 0.0)
            wind += QStringLiteral(" %1 mph").arg(qRound(wx.windMph));
        if (wx.gustMph > 0.0)
            wind += QStringLiteral(" (gust %1)").arg(qRound(wx.gustMph));
        parts << wind;
    }
    if (wx.humidityPct >= 0)
        parts << QStringLiteral("Hum %1%").arg(wx.humidityPct);
    if (wx.pressureMbar > 0.0) {
        parts << QStringLiteral("%1 inHg")
            .arg(wx.pressureMbar * 0.029530, 0, 'f', 2);
    }
    if (wx.rainHourIn > 0.0)
        parts << QStringLiteral("Rain %1\"/hr").arg(wx.rainHourIn, 0, 'f', 2);
    else if (wx.rain24hIn > 0.0)
        parts << QStringLiteral("Rain %1\"/24h").arg(wx.rain24hIn, 0, 'f', 2);
    return parts.join(QStringLiteral("   "));
}

QString packetTypeName(PacketType type)
{
    switch (type) {
    case PacketType::Position:   return QStringLiteral("Position");
    case PacketType::Status:     return QStringLiteral("Status");
    case PacketType::Message:    return QStringLiteral("Message");
    case PacketType::MessageAck: return QStringLiteral("Ack");
    case PacketType::MessageRej: return QStringLiteral("Reject");
    case PacketType::Object:     return QStringLiteral("Object");
    case PacketType::Item:       return QStringLiteral("Item");
    case PacketType::Weather:    return QStringLiteral("Weather");
    case PacketType::Telemetry:  return QStringLiteral("Telemetry");
    case PacketType::Query:      return QStringLiteral("Query");
    case PacketType::Other:      return QStringLiteral("Other");
    }
    return QStringLiteral("Other");
}

} // namespace AetherSDR::aprs
