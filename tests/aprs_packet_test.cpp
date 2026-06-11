// Unit tests for the APRS info-field codec (core/aprs/AprsPacket.*):
// position parsing in uncompressed, compressed and Mic-E form, status,
// messages/acks, the outbound encoders, and the geo helpers. Reference
// vectors come from APRS Protocol Reference 1.0.1 worked examples.

#include "core/aprs/AprsPacket.h"
#include "core/tnc/Ax25.h"

#include <QCoreApplication>
#include <QString>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;
using AetherSDR::ax25::Address;
using AetherSDR::ax25::Frame;

static int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);\
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

static bool near(double a, double b, double tol)
{
    return std::fabs(a - b) <= tol;
}

static Frame makeUiFrame(const QString& src, const QString& dest,
                         const QByteArray& info)
{
    Frame f = Frame::makeUI(*Address::parse(dest), *Address::parse(src),
                            {}, info);
    return f;
}

static void testUncompressedPosition()
{
    // APRS 1.0.1 ch. 8 worked example (no timestamp, no messaging).
    const auto pkt = aprs::parseFrame(makeUiFrame(
        QStringLiteral("N0CALL-9"), QStringLiteral("APRS"),
        QByteArray("!4903.50N/07201.75W-Test 001234")));
    CHECK(pkt.has_value(), "uncompressed position parses");
    if (!pkt)
        return;
    CHECK(pkt->type == aprs::PacketType::Position, "type Position");
    CHECK(pkt->hasPosition, "hasPosition");
    CHECK(near(pkt->latitude, 49.0583, 0.001), "latitude");
    CHECK(near(pkt->longitude, -72.0292, 0.001), "longitude");
    CHECK(pkt->symbolTable == '/' && pkt->symbolCode == '-', "symbol home");
    CHECK(!pkt->messagingCapable, "'!' is not messaging-capable");
    CHECK(pkt->comment == QStringLiteral("Test 001234"), "comment preserved");
    CHECK(pkt->source == QStringLiteral("N0CALL-9"), "source call");
}

static void testTimestampedPositionWithExtensions()
{
    // '@' DTI, 7-char timestamp, course/speed extension and /A= altitude.
    const auto pkt = aprs::parseFrame(makeUiFrame(
        QStringLiteral("W1AW"), QStringLiteral("APRS"),
        QByteArray("@092345z4903.50N/07201.75W>088/036/A=001234Moving east")));
    CHECK(pkt.has_value(), "timestamped position parses");
    if (!pkt)
        return;
    CHECK(pkt->hasPosition, "hasPosition (timestamped)");
    CHECK(pkt->messagingCapable, "'@' is messaging-capable");
    CHECK(near(pkt->courseDeg, 88.0, 0.1), "course extension");
    CHECK(near(pkt->speedKnots, 36.0, 0.1), "speed extension");
    CHECK(pkt->hasAltitude, "altitude extension found");
    CHECK(near(pkt->altitudeFeet, 1234.0, 0.1), "altitude feet");
    CHECK(pkt->comment == QStringLiteral("Moving east"), "comment after extensions");
}

static void testCompressedPosition()
{
    // APRS 1.0.1 ch. 9 worked example: "/5L!!<*e7>{?!" decodes to
    // 49.5 N, 72.75 W with symbol '>' (car).
    const auto pkt = aprs::parseFrame(makeUiFrame(
        QStringLiteral("N0CALL"), QStringLiteral("APRS"),
        QByteArray("!/5L!!<*e7> sTComment")));
    CHECK(pkt.has_value(), "compressed position parses");
    if (!pkt)
        return;
    CHECK(pkt->hasPosition, "hasPosition (compressed)");
    CHECK(near(pkt->latitude, 49.5, 0.01), "compressed latitude");
    CHECK(near(pkt->longitude, -72.75, 0.01), "compressed longitude");
    CHECK(pkt->symbolCode == '>', "compressed symbol code");
    CHECK(pkt->comment == QStringLiteral("Comment"), "compressed comment");
}

static void testMicE()
{
    // APRS 1.0.1 ch. 10 worked example: dest "SSRUVT" encodes 33° 25.64' N
    // (digits 3,3,2,5,6,4; chars 4-6 in P-Z set North / +100 lon / West),
    // info "`(_fn\"Oj/" → 112° 07.74' W, speed 20 kt, course 251°,
    // symbol '/j' (jeep).
    QByteArray info;
    info.append('`');
    info.append('(');
    info.append('_');
    info.append(char(0x66)); // 'f'
    info.append('n');
    info.append('"');
    info.append('O');
    info.append('j');  // symbol code
    info.append('/');  // symbol table
    const auto pkt = aprs::parseFrame(makeUiFrame(
        QStringLiteral("N6XYZ-1"), QStringLiteral("SSRUVT"), info));
    CHECK(pkt.has_value(), "Mic-E parses");
    if (!pkt)
        return;
    CHECK(pkt->type == aprs::PacketType::Position, "Mic-E is a position");
    CHECK(pkt->hasPosition, "Mic-E hasPosition");
    CHECK(near(pkt->latitude, 33.0 + 25.64 / 60.0, 0.001), "Mic-E latitude");
    CHECK(near(pkt->longitude, -(112.0 + 7.74 / 60.0), 0.001), "Mic-E longitude");
    CHECK(near(pkt->speedKnots, 20.0, 0.5), "Mic-E speed");
    CHECK(near(pkt->courseDeg, 251.0, 0.5), "Mic-E course");
    CHECK(pkt->symbolCode == 'j' && pkt->symbolTable == '/', "Mic-E symbol");
}

static void testStatus()
{
    const auto pkt = aprs::parseFrame(makeUiFrame(
        QStringLiteral("N0CALL"), QStringLiteral("APRS"),
        QByteArray(">Net Control Center")));
    CHECK(pkt.has_value() && pkt->type == aprs::PacketType::Status,
          "status parses");
    CHECK(pkt && pkt->comment == QStringLiteral("Net Control Center"),
          "status text");
}

static void testMessages()
{
    // Addressed message with message number.
    const auto msg = aprs::parseFrame(makeUiFrame(
        QStringLiteral("WU2Z"), QStringLiteral("APRS"),
        QByteArray(":KH2Z     :Testing{003")));
    CHECK(msg.has_value() && msg->type == aprs::PacketType::Message,
          "message parses");
    CHECK(msg && msg->addressee == QStringLiteral("KH2Z"), "addressee trimmed");
    CHECK(msg && msg->messageText == QStringLiteral("Testing"), "message text");
    CHECK(msg && msg->messageNo == QStringLiteral("003"), "message number");

    // Ack.
    const auto ack = aprs::parseFrame(makeUiFrame(
        QStringLiteral("KH2Z"), QStringLiteral("APRS"),
        QByteArray(":WU2Z     :ack003")));
    CHECK(ack.has_value() && ack->type == aprs::PacketType::MessageAck,
          "ack parses");
    CHECK(ack && ack->messageNo == QStringLiteral("003"), "ack number");

    // Rej.
    const auto rej = aprs::parseFrame(makeUiFrame(
        QStringLiteral("KH2Z"), QStringLiteral("APRS"),
        QByteArray(":WU2Z     :rej003")));
    CHECK(rej.has_value() && rej->type == aprs::PacketType::MessageRej,
          "rej parses");

    // Unnumbered message.
    const auto plain = aprs::parseFrame(makeUiFrame(
        QStringLiteral("WU2Z"), QStringLiteral("APRS"),
        QByteArray(":KH2Z     :Hello")));
    CHECK(plain && plain->type == aprs::PacketType::Message
              && plain->messageNo.isEmpty(),
          "unnumbered message");
}

static void testEncoders()
{
    const QString pos = aprs::encodeUncompressedPosition(
        49.0583333, -72.0291667, '/', '-', QStringLiteral("Test"), true);
    CHECK(pos == QStringLiteral("=4903.50N/07201.75W-Test"),
          "position encoder round-trips the reference vector");

    // Encoder output must parse back to the same coordinates.
    const auto rt = aprs::parseFrame(makeUiFrame(
        QStringLiteral("N0CALL"), QStringLiteral("APRS"), pos.toLatin1()));
    CHECK(rt && rt->hasPosition && rt->messagingCapable,
          "encoded position parses back");
    CHECK(rt && near(rt->latitude, 49.0583, 0.001)
              && near(rt->longitude, -72.0292, 0.001),
          "round-trip coordinates");

    CHECK(aprs::encodeMessage(QStringLiteral("KH2Z"), QStringLiteral("Testing"),
                              QStringLiteral("3"))
              == QStringLiteral(":KH2Z     :Testing{3"),
          "message encoder pads addressee to 9");
    CHECK(aprs::encodeAck(QStringLiteral("WU2Z"), QStringLiteral("003"))
              == QStringLiteral(":WU2Z     :ack003"),
          "ack encoder");
    CHECK(aprs::encodeStatus(QStringLiteral("hi")) == QStringLiteral(">hi"),
          "status encoder");
}

static void testGeoHelpers()
{
    CHECK(aprs::gridSquare(48.27, -116.56) == QStringLiteral("DN18rg"),
          "grid square matches the radio's own GPS grid");
    // Newington CT → Seattle WA: 2428.9 statute miles by haversine on the
    // mean Earth radius, initial bearing ~285°.
    const double mi = aprs::distanceMiles(41.71, -72.73, 47.61, -122.33);
    CHECK(near(mi, 2428.9, 5.0), "great-circle distance");
    const double brg = aprs::bearingDeg(41.71, -72.73, 47.61, -122.33);
    CHECK(brg > 280.0 && brg < 300.0, "initial bearing");
    CHECK(aprs::symbolDescription('/', '-') == QStringLiteral("Home"),
          "symbol description");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testUncompressedPosition();
    testTimestampedPositionWithExtensions();
    testCompressedPosition();
    testMicE();
    testStatus();
    testMessages();
    testEncoders();
    testGeoHelpers();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("aprs_packet_test: all tests passed\n");
    return 0;
}
