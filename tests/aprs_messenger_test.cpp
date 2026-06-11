// Unit tests for the APRS messaging engine (core/aprs/AprsMessenger.*):
// outgoing message numbering + ack matching, incoming auto-ack and duplicate
// suppression, unread tracking, and the station roster's digipeat dedupe
// (core/aprs/AprsStationList.*). Pure protocol layer — no DSP, no radio.

#include "core/aprs/AprsMessenger.h"
#include "core/aprs/AprsPacket.h"
#include "core/aprs/AprsStationList.h"
#include "core/tnc/Ax25.h"

#include <QCoreApplication>
#include <QVector>

#include <cstdio>
#include <optional>

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

static aprs::Packet parseInfo(const QString& src, const QByteArray& info)
{
    const Frame f = Frame::makeUI(*Address::parse(QStringLiteral("APRS")),
                                  *Address::parse(src), {}, info);
    const auto pkt = aprs::parseFrame(f);
    return pkt.value_or(aprs::Packet{});
}

static void testOutgoingAckFlow()
{
    AprsMessenger messenger;
    messenger.setMyAddress(*Address::parse(QStringLiteral("N0CALL-9")));

    QVector<QByteArray> sentFrames;
    QObject::connect(&messenger, &AprsMessenger::transmitFrame,
                     [&sentFrames](const QByteArray& raw) {
        sentFrames.append(raw);
    });

    CHECK(messenger.sendMessage(QStringLiteral("W1AW"),
                                QStringLiteral("Hello there")),
          "sendMessage accepts a valid destination");
    CHECK(sentFrames.size() == 1, "one frame transmitted");
    CHECK(!messenger.sendMessage(QStringLiteral(""), QStringLiteral("x")),
          "empty destination rejected");
    CHECK(!messenger.sendMessage(QStringLiteral("TOOLONGCALL"),
                                 QStringLiteral("x")),
          "invalid destination rejected");

    // The transmitted frame must decode to the expected APRS message.
    const auto decoded = Frame::decode(sentFrames.first());
    CHECK(decoded.has_value(), "transmitted frame decodes");
    if (decoded) {
        const auto pkt = aprs::parseFrame(*decoded);
        CHECK(pkt && pkt->type == aprs::PacketType::Message,
              "transmitted frame is an APRS message");
        CHECK(pkt && pkt->addressee == QStringLiteral("W1AW"),
              "addressee on the air");
        CHECK(pkt && pkt->messageText == QStringLiteral("Hello there"),
              "text on the air");
        CHECK(pkt && !pkt->messageNo.isEmpty(), "message number assigned");
    }

    auto msgs = messenger.messages();
    CHECK(msgs.size() == 1, "one message stored");
    CHECK(msgs.first().state == AprsMessenger::State::Sent, "state Sent");
    const QString msgNo = msgs.first().msgNo;

    // Ack from the wrong station must not match.
    messenger.onPacket(parseInfo(QStringLiteral("K1ABC"),
        QStringLiteral(":N0CALL-9 :ack%1").arg(msgNo).toLatin1()));
    CHECK(messenger.messages().first().state == AprsMessenger::State::Sent,
          "ack from wrong station ignored");

    // Ack from the right station closes it out.
    messenger.onPacket(parseInfo(QStringLiteral("W1AW"),
        QStringLiteral(":N0CALL-9 :ack%1").arg(msgNo).toLatin1()));
    CHECK(messenger.messages().first().state == AprsMessenger::State::Acked,
          "ack from the destination marks Acked");
}

static void testIncomingAutoAckAndDedupe()
{
    AprsMessenger messenger;
    messenger.setMyAddress(*Address::parse(QStringLiteral("N0CALL-9")));

    QVector<QByteArray> sentFrames;
    int received = 0;
    QObject::connect(&messenger, &AprsMessenger::transmitFrame,
                     [&sentFrames](const QByteArray& raw) {
        sentFrames.append(raw);
    });
    QObject::connect(&messenger, &AprsMessenger::messageReceived,
                     [&received](const AprsMessenger::Message&) { ++received; });

    // Message addressed to someone else: ignored entirely.
    messenger.onPacket(parseInfo(QStringLiteral("W1AW"),
                                 QByteArray(":K1ABC    :not for us{001")));
    CHECK(received == 0 && sentFrames.isEmpty() && messenger.messages().isEmpty(),
          "message for another station ignored");

    // Message for us: stored unread and auto-acked.
    messenger.onPacket(parseInfo(QStringLiteral("W1AW"),
                                 QByteArray(":N0CALL-9 :QSL?{042")));
    CHECK(received == 1, "messageReceived emitted");
    CHECK(messenger.unreadCount() == 1, "unread count 1");
    CHECK(sentFrames.size() == 1, "auto-ack transmitted");
    if (!sentFrames.isEmpty()) {
        const auto ackFrame = Frame::decode(sentFrames.first());
        std::optional<aprs::Packet> ack;
        if (ackFrame)
            ack = aprs::parseFrame(*ackFrame);
        CHECK(ack && ack->type == aprs::PacketType::MessageAck
                  && ack->messageNo == QStringLiteral("042")
                  && ack->addressee == QStringLiteral("W1AW"),
              "auto-ack carries the sender's message number");
    }

    // The sender retries the same {042}: re-ack but don't store a duplicate.
    messenger.onPacket(parseInfo(QStringLiteral("W1AW"),
                                 QByteArray(":N0CALL-9 :QSL?{042")));
    CHECK(received == 1, "duplicate not re-delivered");
    CHECK(messenger.messages().size() == 1, "duplicate not stored");
    CHECK(sentFrames.size() == 2, "duplicate still re-acked");

    messenger.markAllRead();
    CHECK(messenger.unreadCount() == 0, "markAllRead clears the badge");

    // Our own digipeated frame must not loop back into the store.
    messenger.onPacket(parseInfo(QStringLiteral("N0CALL-9"),
                                 QByteArray(":N0CALL-9 :echo{077")));
    CHECK(messenger.messages().size() == 1, "own digipeated frame ignored");
}

static void testStationListDedupe()
{
    AprsStationList stations;
    const aprs::Packet pkt = parseInfo(
        QStringLiteral("W1AW-1"),
        QByteArray("!4903.50N/07201.75W-Direct copy"));

    CHECK(stations.record(pkt), "first copy recorded");
    CHECK(!stations.record(pkt), "digipeated duplicate dropped");
    CHECK(stations.size() == 1, "one station");
    const auto s = stations.find(QStringLiteral("W1AW-1"));
    CHECK(s.has_value(), "station findable");
    CHECK(s && s->packets == 1, "duplicate did not inflate the packet count");
    CHECK(s && s->hasPosition, "position merged");

    // A different payload from the same station is new traffic.
    CHECK(stations.record(parseInfo(QStringLiteral("W1AW-1"),
                                    QByteArray(">Status text"))),
          "new payload recorded");
    const auto s2 = stations.find(QStringLiteral("W1AW-1"));
    CHECK(s2 && s2->packets == 2, "packet count advanced");
    CHECK(s2 && s2->status == QStringLiteral("Status text"), "status merged");
    CHECK(s2 && s2->hasPosition, "status did not clobber the position");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testOutgoingAckFlow();
    testIncomingAutoAckAndDedupe();
    testStationListDedupe();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("aprs_messenger_test: all tests passed\n");
    return 0;
}
