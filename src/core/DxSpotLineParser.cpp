#include "core/DxSpotLineParser.h"

#include <QRegularExpression>

namespace AetherSDR::DxSpotLineParser {

namespace {

// GoCluster spot/spot.go: the tail is the last 15 columns of every line,
// whatever the operator's configured line length.
constexpr int kGoClusterTailLength = 15;

bool parseHhmm(const QString& hhmm, QTime& out)
{
    if (hhmm.size() != 4) {
        return false;
    }
    for (QChar c : hhmm) {
        if (!c.isDigit()) {
            return false;
        }
    }
    out = QTime(hhmm.left(2).toInt(), hhmm.mid(2, 2).toInt());
    return true;
}

// Split GoCluster's tail off `line`, filling grid/confidence/path/time and
// returning the head (everything before the tail). Every fixed column is
// checked, so a line that merely looks similar is left to the generic parser.
bool splitGoClusterTail(const QString& line, QString& head, DxSpot& spot)
{
    const int n = line.size();
    if (n <= kGoClusterTailLength) {
        return false;
    }
    auto fromEnd = [&](int i) { return line.at(n - i); };

    if (fromEnd(15) != ' ' || fromEnd(13) != ' ' || fromEnd(8) != ' '
            || fromEnd(6) != ' ' || fromEnd(1).toUpper() != 'Z') {
        return false;
    }

    const QChar glyph = fromEnd(14);
    if (!QStringLiteral(" ><=-#").contains(glyph)) {
        return false;
    }

    const QChar conf = fromEnd(7);
    if (!QStringLiteral(" ?SPVCB").contains(conf)) {
        return false;
    }

    static const QRegularExpression gridRx(QStringLiteral("^[A-Ra-r]{2}[0-9]{2}$"));
    const QString grid = line.mid(n - 12, 4);
    if (grid != QStringLiteral("    ") && !gridRx.match(grid).hasMatch()) {
        return false;
    }

    QTime time;
    if (!parseHhmm(line.mid(n - 5, 4), time)) {
        return false;
    }

    head = line.left(n - kGoClusterTailLength);
    spot.utcTime = time;
    spot.dxGrid = grid.trimmed();
    spot.confidence = conf == ' ' ? QChar() : conf;
    spot.pathGlyph = glyph == ' ' ? QChar() : glyph;
    return true;
}

void finishComment(const QString& raw, DxSpot& spot)
{
    // Fixed-width servers pad inside the comment; every inner space would
    // otherwise reach the radio as its own 0x7f in "spot add comment=".
    spot.comment = raw.simplified();
    spot.hasSnr = extractReportSnr(spot.comment, spot.snr);
}

} // namespace

bool isGoClusterBanner(const QString& line)
{
    return line.contains(QStringLiteral("GoCluster"), Qt::CaseInsensitive);
}

bool parseSpotLine(const QString& line, DxSpot& spot, bool goCluster)
{
    QString head;
    if (goCluster && splitGoClusterTail(line, head, spot)) {
        static const QRegularExpression headRx(
            QStringLiteral(R"(^DX\s+de\s+(\S+?):\s+(\d+\.?\d*)\s+(\S+)(.*)$)"),
            QRegularExpression::CaseInsensitiveOption);
        const auto match = headRx.match(head);
        if (match.hasMatch()) {
            spot.spotterCall = match.captured(1);
            spot.freqMhz = match.captured(2).toDouble() / 1000.0;
            spot.dxCall = match.captured(3);
            finishComment(match.captured(4), spot);
            return spot.freqMhz > 0.0 && !spot.dxCall.isEmpty();
        }
        spot.dxGrid.clear();
        spot.confidence = QChar();
        spot.pathGlyph = QChar();
    }

    // Standard format: DX de W3LPL:     14025.0  JA1ABC       CW big signal       1824Z
    // Z is the terminator — ignore any trailing chars (some nodes append BEL/NUL)
    static const QRegularExpression rx(
        QStringLiteral(R"(^DX\s+de\s+(\S+?):\s+(\d+\.?\d*)\s+(\S+)\s+(.*?)\s+(\d{4})Z)"),
        QRegularExpression::CaseInsensitiveOption);

    const auto match = rx.match(line);
    if (!match.hasMatch()) {
        return false;
    }

    spot.spotterCall = match.captured(1);
    spot.freqMhz = match.captured(2).toDouble() / 1000.0;
    spot.dxCall = match.captured(3);
    finishComment(match.captured(4), spot);
    parseHhmm(match.captured(5), spot.utcTime);

    return spot.freqMhz > 0.0 && !spot.dxCall.isEmpty();
}

bool extractReportSnr(const QString& comment, int& snr)
{
    static const QRegularExpression rx(
        QStringLiteral(R"(^[A-Za-z][A-Za-z0-9]{0,7}\s+([+-]?\d{1,2})\s*dB\b)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = rx.match(comment);
    if (!match.hasMatch()) {
        return false;
    }
    snr = match.captured(1).toInt();
    return true;
}

} // namespace AetherSDR::DxSpotLineParser
