#include "core/tnc/Ax25FrameFormatter.h"

#include <QByteArray>
#include <QDateTime>
#include <QStringList>

#include <cstdio>
#include <optional>

using namespace MasterSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok)
        ++g_failed;
}

void appendAddress(QByteArray& out, const QString& callsign, int ssid, bool last)
{
    const QString base = callsign.left(6).toUpper();
    for (int i = 0; i < 6; ++i) {
        const char ch = (i < base.size()) ? base.at(i).toLatin1() : ' ';
        out.append(static_cast<char>(static_cast<quint8>(ch) << 1));
    }
    quint8 ssidByte = 0x60u | static_cast<quint8>((ssid & 0x0f) << 1);
    if (last)
        ssidByte |= 0x01u;
    out.append(static_cast<char>(ssidByte));
}

quint16 fcs(const QByteArray& bytes)
{
    quint16 crc = 0xffff;
    for (char ch : bytes) {
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

QByteArray makeUiFrame(const QByteArray& payload)
{
    QByteArray frame;
    appendAddress(frame, QStringLiteral("APRS"), 0, false);
    appendAddress(frame, QStringLiteral("N0CALL"), 9, false);
    appendAddress(frame, QStringLiteral("WIDE1"), 1, false);
    appendAddress(frame, QStringLiteral("WIDE2"), 1, true);
    frame.append(char(0x03));
    frame.append(char(0xf0));
    frame.append(payload);
    const quint16 crc = fcs(frame);
    frame.append(static_cast<char>(crc & 0xff));
    frame.append(static_cast<char>((crc >> 8) & 0xff));
    return frame;
}

void testUiFrameDisplayFields()
{
    const auto timestamp = QDateTime::fromString(QStringLiteral("2026-05-16T20:15:03Z"),
                                                 Qt::ISODate);
    const auto decoded = Ax25FrameFormatter::decodeFrameBytes(
        makeUiFrame("hello world"), timestamp, 0.8);

    report("valid UI frame decodes", decoded.has_value());
    if (!decoded)
        return;
    report("source SSID", decoded->source == QStringLiteral("N0CALL-9"));
    report("destination callsign", decoded->destination == QStringLiteral("APRS"));
    report("digipeater path", decoded->path == QStringList({QStringLiteral("WIDE1-1"), QStringLiteral("WIDE2-1")}));
    report("UI control/PID", decoded->isUiFrame && decoded->control == 0x03 && decoded->pid == 0xf0);
    report("printable payload", decoded->payloadText == QStringLiteral("hello world"));
    report("FCS accepted", decoded->fcsOk);
}

void testBinaryPayloadFallsBackToHex()
{
    const QByteArray payload = QByteArray::fromHex("0102ff");
    const auto decoded = Ax25FrameFormatter::decodeFrameBytes(makeUiFrame(payload));
    report("binary payload frame decodes", decoded.has_value());
    if (!decoded)
        return;
    report("binary payload text suppressed", decoded->payloadText.isEmpty());
    report("binary payload hex", decoded->payloadHex == QStringLiteral("01 02 FF"));
}

void testBadFcsRejected()
{
    QByteArray frame = makeUiFrame("bad fcs");
    frame[frame.size() - 1] = static_cast<char>(static_cast<quint8>(frame.at(frame.size() - 1)) ^ 0x40u);
    const auto decoded = Ax25FrameFormatter::decodeFrameBytes(frame);
    report("bad FCS rejected", !decoded.has_value());
}

} // namespace

int main()
{
    testUiFrameDisplayFields();
    testBinaryPayloadFallsBackToHex();
    testBadFcsRejected();

    std::printf("\n%s\n", g_failed == 0 ? "All tests passed." : "Some tests failed.");
    return g_failed == 0 ? 0 : 1;
}
