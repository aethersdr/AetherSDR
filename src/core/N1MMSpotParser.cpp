#include "N1MMSpotParser.h"
#include "models/BandSettings.h"

#include <QXmlStreamReader>
#include <QStringList>
#include <QTimeZone>

namespace AetherSDR::N1MMSpotParser {

bool parsePacket(const QByteArray& data, N1mmSpot& outSpot, QString& outAction)
{
    QXmlStreamReader xml(data);

    QString currentTag;
    QString dxcall, freqStr, mode, spottercall, comment, stationName,
            action, status, statuslist, timestampStr;
    bool sawSpotElement = false;

    while (!xml.atEnd() && !xml.hasError()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            currentTag = xml.name().toString();
            if (currentTag.compare("spot", Qt::CaseInsensitive) == 0)
                sawSpotElement = true;
        } else if (token == QXmlStreamReader::EndElement) {
            // Without this, stray text between one element closing and the
            // next opening is appended to the element that just closed —
            // e.g. junk after </frequency> concatenates onto the number and
            // makes toDouble() fail on an otherwise-valid packet.
            currentTag.clear();
        } else if (token == QXmlStreamReader::Characters && !xml.isWhitespace()) {
            const QString text = xml.text().toString();
            if (currentTag.compare("dxcall", Qt::CaseInsensitive) == 0) dxcall += text;
            else if (currentTag.compare("frequency", Qt::CaseInsensitive) == 0) freqStr += text;
            else if (currentTag.compare("mode", Qt::CaseInsensitive) == 0) mode += text;
            else if (currentTag.compare("spottercall", Qt::CaseInsensitive) == 0) spottercall += text;
            else if (currentTag.compare("comment", Qt::CaseInsensitive) == 0) comment += text;
            else if (currentTag.compare("stationname", Qt::CaseInsensitive) == 0) stationName += text;
            else if (currentTag.compare("action", Qt::CaseInsensitive) == 0) action += text;
            else if (currentTag.compare("status", Qt::CaseInsensitive) == 0) status += text;
            else if (currentTag.compare("statuslist", Qt::CaseInsensitive) == 0) statuslist += text;
            else if (currentTag.compare("timestamp", Qt::CaseInsensitive) == 0) timestampStr += text;
        }
    }

    // The loop also exits on hasError(), so without this a document the reader
    // gave up on still succeeds as long as <spot>, a dxcall and some digits of
    // <frequency> arrived first — e.g. a datagram cut mid-number at
    // "<frequency>140" would yield a marker at 0.140 MHz, on the wrong band and
    // under the wrong spotKey(). onReadyRead()'s 64 KB cap and ordinary UDP
    // fragment loss both produce exactly that shape.
    if (xml.hasError() || !sawSpotElement)
        return false;

    dxcall = dxcall.trimmed();
    if (dxcall.isEmpty())
        return false;

    bool freqOk = false;
    const double freqKhz = freqStr.trimmed().toDouble(&freqOk);
    if (!freqOk || freqKhz <= 0.0)
        return false;

    outAction = action.trimmed().toLower();
    if (outAction.isEmpty())
        outAction = "add";
    if (outAction != "add" && outAction != "delete")
        return false;

    N1mmSpot spot;
    spot.dxCall      = dxcall;
    spot.freqMhz     = freqKhz / 1000.0;   // wire format is kHz
    spot.mode        = mode.trimmed().toUpper();
    spot.spotterCall = spottercall.trimmed();
    spot.comment     = comment.trimmed();
    spot.stationName = stationName.trimmed();
    spot.statusRaw   = (!statuslist.trimmed().isEmpty() ? statuslist : status).trimmed();
    spot.statusFlag  = normalizeStatusFlag(spot.statusRaw);

    const QString ts = timestampStr.trimmed();
    if (!ts.isEmpty()) {
        QDateTime dt = QDateTime::fromString(ts, "yyyy-MM-dd HH:mm:ss");
        if (dt.isValid()) {
            // QTimeZone::utc() rather than the Qt 6.5+ QTimeZone::UTC constant —
            // the Linux CI image builds against an older Qt 6.
            dt.setTimeZone(QTimeZone::utc());
            spot.timestamp = dt;
        }
    }

    outSpot = spot;
    return true;
}

QString documentRoot(const QByteArray& data)
{
    QXmlStreamReader xml(data);
    while (!xml.atEnd() && !xml.hasError()) {
        if (xml.readNext() == QXmlStreamReader::StartElement)
            return xml.name().toString();
    }
    return {};
}

QString spotKey(const QString& dxCall, double freqMhz)
{
    return dxCall.trimmed().toUpper() + "|" + BandSettings::bandForFrequency(freqMhz);
}

QString normalizeStatusFlag(const QString& statusList)
{
    if (statusList.isEmpty())
        return {};

    const QStringList tokens = statusList.toLower().split(' ', Qt::SkipEmptyParts);
    auto has = [&](const char* needle) { return tokens.contains(QLatin1String(needle)); };

    // Priority: what a contest op most needs to notice first.
    if (has("bust"))  return "bust";
    if (has("dupe"))  return "dupe";
    if (has("mult"))  return "mult";   // covers both "single mult" and "double mult"
    if (has("cq"))    return "cq";
    if (has("busy"))  return "busy";
    if (has("qtc"))   return "qtc";
    if (has("new"))   return "new";   // "new qso"
    return {};
}

} // namespace AetherSDR::N1MMSpotParser
