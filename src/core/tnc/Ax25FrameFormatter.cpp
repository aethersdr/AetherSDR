#include "core/tnc/Ax25FrameFormatter.h"

#include <QChar>

#include <algorithm>

namespace MasterSDR {

namespace {

QString decodeAddress(const QByteArray& frame, int offset)
{
    QString callsign;
    callsign.reserve(6);
    for (int i = 0; i < 6; ++i) {
        const char ch = static_cast<char>(
            static_cast<unsigned char>(frame.at(offset + i)) >> 1);
        if (ch != ' ')
            callsign.append(QLatin1Char(ch));
    }

    const quint8 ssid = (static_cast<quint8>(frame.at(offset + 6)) >> 1) & 0x0f;
    if (ssid != 0)
        callsign.append(QStringLiteral("-%1").arg(ssid));
    return callsign;
}

bool isLastAddress(const QByteArray& frame, int offset)
{
    return (static_cast<quint8>(frame.at(offset + 6)) & 0x01u) != 0;
}

QString byteHex(quint8 value)
{
    return QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0')).toUpper();
}

} // namespace

std::optional<Ax25DecodedFrame> Ax25FrameFormatter::decodeFrameBytes(
    const QByteArray& frameWithFcs,
    const QDateTime& timestampUtc,
    double quality)
{
    if (frameWithFcs.size() < 18 || !hasValidFcs(frameWithFcs))
        return std::nullopt;

    int offset = 0;
    const QString destination = decodeAddress(frameWithFcs, offset);
    const bool destLast = isLastAddress(frameWithFcs, offset);
    offset += 7;
    if (destLast || offset + 7 > frameWithFcs.size() - 2)
        return std::nullopt;

    const QString source = decodeAddress(frameWithFcs, offset);
    bool last = isLastAddress(frameWithFcs, offset);
    offset += 7;

    QStringList path;
    while (!last) {
        if (offset + 7 > frameWithFcs.size() - 2)
            return std::nullopt;
        path.append(decodeAddress(frameWithFcs, offset));
        last = isLastAddress(frameWithFcs, offset);
        offset += 7;
    }

    if (offset >= frameWithFcs.size() - 2)
        return std::nullopt;

    Ax25DecodedFrame frame;
    frame.timestampUtc = timestampUtc.toUTC();
    frame.source = source;
    frame.destination = destination;
    frame.path = path;
    frame.control = static_cast<quint8>(frameWithFcs.at(offset++));
    frame.pid = 0;

    const bool carriesPid = (frame.control == 0x03) || ((frame.control & 0x01u) == 0);
    if (carriesPid) {
        if (offset >= frameWithFcs.size() - 2)
            return std::nullopt;
        frame.pid = static_cast<quint8>(frameWithFcs.at(offset++));
    }

    frame.payload = frameWithFcs.mid(offset, frameWithFcs.size() - offset - 2);
    frame.payloadText = payloadText(frame.payload);
    frame.payloadHex = payloadHex(frame.payload);
    frame.isUiFrame = (frame.control == 0x03 && frame.pid == 0xf0);
    frame.fcsOk = true;
    frame.confidenceOrQuality = quality;
    return frame;
}

QString Ax25FrameFormatter::formatLogLine(const Ax25DecodedFrame& frame)
{
    const QString time = frame.timestampUtc.toUTC().toString(Qt::ISODate);
    const QString via = frame.path.isEmpty()
        ? QStringLiteral("-")
        : frame.path.join(QLatin1Char(','));
    const QString body = frame.payloadText.isEmpty()
        ? QStringLiteral("hex:") + frame.payloadHex
        : frame.payloadText;
    return QStringLiteral("%1  SRC=%2  DST=%3  VIA=%4  %5 pid=%6  >%7")
        .arg(time,
             frame.source,
             frame.destination,
             via,
             frame.isUiFrame ? QStringLiteral("UI") : QStringLiteral("CTRL=%1").arg(byteHex(frame.control)),
             byteHex(frame.pid),
             body);
}

QString Ax25FrameFormatter::payloadText(const QByteArray& payload)
{
    QString text;
    text.reserve(payload.size());
    for (const char ch : payload) {
        const uchar byte = static_cast<uchar>(ch);
        if (byte == '\r' || byte == '\n' || byte == '\t') {
            text.append(QLatin1Char(static_cast<char>(byte)));
        } else if (byte >= 0x20 && byte <= 0x7e) {
            text.append(QLatin1Char(static_cast<char>(byte)));
        } else {
            return {};
        }
    }
    return text;
}

QString Ax25FrameFormatter::payloadHex(const QByteArray& payload)
{
    return QString::fromLatin1(payload.toHex(' ').toUpper());
}

bool Ax25FrameFormatter::hasValidFcs(const QByteArray& frameWithFcs)
{
    if (frameWithFcs.size() < 3)
        return false;
    const QByteArray body = frameWithFcs.left(frameWithFcs.size() - 2);
    const quint16 actual = computeFcs(body);
    const quint16 expected =
        static_cast<quint8>(frameWithFcs.at(frameWithFcs.size() - 2)) |
        (static_cast<quint16>(static_cast<quint8>(frameWithFcs.at(frameWithFcs.size() - 1))) << 8);
    return actual == expected;
}

quint16 Ax25FrameFormatter::computeFcs(const QByteArray& bytes)
{
    quint16 crc = 0xffff;
    for (const char ch : bytes) {
        quint8 byte = static_cast<quint8>(ch);
        for (int i = 0; i < 8; ++i) {
            const bool xorIn = ((crc ^ byte) & 0x0001u) != 0;
            crc >>= 1;
            if (xorIn)
                crc ^= 0x8408u;
            byte >>= 1;
        }
    }
    return static_cast<quint16>(crc ^ 0xffffu);
}

} // namespace MasterSDR
