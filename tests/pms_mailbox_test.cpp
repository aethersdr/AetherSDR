// Unit + integration tests for the AX.25 connected-mode data link and the
// Personal Mailbox System (PMS). These exercise the protocol layer in isolation
// (no DSP / radio), driving the mailbox exactly as a remote caller's TNC would.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/pms/PmsMailbox.h"
#include "core/tnc/Ax25.h"
#include "core/tnc/Ax25Connection.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QVector>
#include <QtGlobal>

#include <cstdio>

using namespace AetherSDR;
using AetherSDR::ax25::Address;
using AetherSDR::ax25::Frame;
using AetherSDR::ax25::FrameType;

static int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);\
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

static void testAddress()
{
    auto a = Address::parse(QStringLiteral("N0CALL-7"));
    CHECK(a.has_value(), "parse N0CALL-7");
    CHECK(a && a->call == QLatin1String("N0CALL") && a->ssid == 7, "ssid parsed");
    CHECK(a && a->toString() == QLatin1String("N0CALL-7"), "toString with ssid");

    auto b = Address::parse(QStringLiteral("w1aw"));
    CHECK(b && b->call == QLatin1String("W1AW") && b->ssid == 0, "uppercased, ssid 0");
    CHECK(b && b->toString() == QLatin1String("W1AW"), "toString no ssid");

    CHECK(!Address::parse(QStringLiteral("")).has_value(), "empty rejected");
    CHECK(!Address::parse(QStringLiteral("TOOLONGCALL")).has_value(), "overlong rejected");
    // AX.25 limits the base callsign to 6 characters, so a 7-char vanity such as
    // "AETHBBS" is not a legal address — callers must use <= 6 (e.g. "AETBBS").
    CHECK(!Address::parse(QStringLiteral("AETHBBS")).has_value(), "7-char alias rejected");
    CHECK(Address::parse(QStringLiteral("AETBBS")).has_value(), "6-char alias accepted");
}

static void testFrameRoundTrip()
{
    const Address dst{QStringLiteral("N0PMS"), 1, false, false};
    const Address src{QStringLiteral("K7ABC"), 0, false, false};

    {
        Frame s = Frame::makeU(dst, src, FrameType::SABM, /*pf=*/true, /*cmd=*/true);
        auto d = Frame::decode(s.encode());
        CHECK(d && d->type == FrameType::SABM, "SABM type");
        CHECK(d && d->dest == dst && d->src == src, "SABM addresses");
        CHECK(d && d->pollFinal && d->command, "SABM P + command");
    }
    {
        Frame i = Frame::makeI(dst, src, /*ns=*/3, /*nr=*/5, /*pf=*/false,
                               QByteArray("hello"));
        auto d = Frame::decode(i.encode());
        CHECK(d && d->type == FrameType::I, "I type");
        CHECK(d && d->ns == 3 && d->nr == 5, "I sequence numbers");
        CHECK(d && d->info == QByteArray("hello") && d->command, "I info + command");
    }
    {
        Frame r = Frame::makeS(dst, src, FrameType::RR, /*nr=*/2, /*pf=*/true,
                               /*cmd=*/false);
        auto d = Frame::decode(r.encode());
        CHECK(d && d->type == FrameType::RR && d->nr == 2, "RR type + nr");
        CHECK(d && d->pollFinal && !d->command, "RR final + response");
    }
    {
        Frame ui = Frame::makeUI(Address{QStringLiteral("BEACON"), 0, false, false}, src,
                                 {Address{QStringLiteral("WIDE1"), 1, false, false}},
                                 QByteArray("hi"));
        auto d = Frame::decode(ui.encode());
        CHECK(d && d->type == FrameType::UI, "UI type");
        CHECK(d && d->via.size() == 1 && d->via.at(0).call == QLatin1String("WIDE1")
                  && d->via.at(0).ssid == 1, "UI via path");
        CHECK(d && d->info == QByteArray("hi"), "UI info");
    }
}

static void testConnection()
{
    Ax25Connection conn;
    const Address local{QStringLiteral("N0PMS"), 1, false, false};
    const Address peer{QStringLiteral("K7ABC"), 0, false, false};
    conn.setLocalAddress(local);

    QVector<QByteArray> tx;
    bool connected = false;
    bool disconnected = false;
    QByteArray rxData;
    QObject::connect(&conn, &Ax25Connection::sendFrame,
                     [&](const QByteArray& f) { tx.append(f); });
    QObject::connect(&conn, &Ax25Connection::connected,
                     [&](const Address&) { connected = true; });
    QObject::connect(&conn, &Ax25Connection::disconnected,
                     [&](const Address&, bool) { disconnected = true; });
    QObject::connect(&conn, &Ax25Connection::dataReceived,
                     [&](const QByteArray& d) { rxData = d; });

    conn.onFrameReceived(Frame::makeU(local, peer, FrameType::SABM, true, true));
    CHECK(connected, "connected after SABM");
    bool sawUA = false;
    for (const QByteArray& f : tx) {
        auto d = Frame::decode(f);
        if (d && d->type == FrameType::UA)
            sawUA = true;
    }
    CHECK(sawUA, "UA emitted in response to SABM");

    // A frame for someone else must be ignored.
    const Address other{QStringLiteral("N9XYZ"), 2, false, false};
    conn.onFrameReceived(Frame::makeU(other, peer, FrameType::SABM, true, true));

    conn.onFrameReceived(Frame::makeI(local, peer, 0, 0, true, QByteArray("PING")));
    CHECK(rxData == QByteArray("PING"), "I-frame data delivered");

    conn.onFrameReceived(Frame::makeU(local, peer, FrameType::DISC, true, true));
    CHECK(disconnected, "disconnected after DISC");
}

// Regression: on a half-duplex radio link the send window must be 1 — only one
// unacknowledged I-frame in flight at a time. A multi-frame reply must drain
// one-frame-per-ack, NOT blast several back-to-back. (Observed live 2026-05-30:
// a 3-frame LIST reply went out as 3 back-to-back PTT keyups; the peer's ack
// arrived while we were still transmitting, so we never heard it and stalled
// into a T1 retransmit loop until link failure. A single-frame INFO reply with a
// clean listen window after it worked fine.)
static void testHalfDuplexWindowOneDrainsMultiFrame()
{
    Ax25Connection conn;
    const Address local{QStringLiteral("N0PMS"), 1, false, false};
    const Address peer{QStringLiteral("K7ABC"), 0, false, false};
    conn.setLocalAddress(local);
    conn.setPaclen(128);

    QVector<QByteArray> tx;
    QObject::connect(&conn, &Ax25Connection::sendFrame,
                     [&](const QByteArray& f) { tx.append(f); });

    conn.onFrameReceived(Frame::makeU(local, peer, FrameType::SABM, true, true));
    tx.clear(); // drop the UA; we only care about I-frames below

    auto iFrameNs = [&]() {
        QVector<int> ns; // N(S) of every I-frame emitted so far
        for (const QByteArray& f : tx) {
            auto d = Frame::decode(f);
            if (d && d->type == FrameType::I)
                ns.append(d->ns);
        }
        return ns;
    };

    // A reply that needs three I-frames (128 + 128 + 44 bytes at paclen 128).
    conn.sendData(QByteArray(300, 'X'));
    CHECK(iFrameNs().size() == 1, "window=1: only one I-frame in flight initially");
    CHECK(iFrameNs().value(0) == 0, "first I-frame is N(S)=0");

    // Peer acks the first (standalone RR N(R)=1) -> the next frame may go.
    conn.onFrameReceived(Frame::makeS(local, peer, FrameType::RR, 1, false, false));
    CHECK(iFrameNs().size() == 2, "second I-frame goes only after the first is acked");
    CHECK(iFrameNs().value(1) == 1, "second I-frame is N(S)=1");

    conn.onFrameReceived(Frame::makeS(local, peer, FrameType::RR, 2, false, false));
    CHECK(iFrameNs().size() == 3, "third I-frame goes only after the second is acked");
    CHECK(iFrameNs().value(2) == 2, "third I-frame is N(S)=2");

    // Final ack drains the window with no retransmit storm.
    conn.onFrameReceived(Frame::makeS(local, peer, FrameType::RR, 3, false, false));
    const QVector<int> all = iFrameNs();
    CHECK(all.size() == 3, "exactly three I-frames sent, none retransmitted");
    CHECK(all.count(0) == 1 && all.count(1) == 1 && all.count(2) == 1,
          "each I-frame transmitted exactly once (no duplicate/retransmit storm)");
}

namespace {
// Drives the mailbox from the perspective of a remote caller's TNC.
struct Peer {
    PmsMailbox* pms{nullptr};
    Address local;
    Address peer;
    QVector<QByteArray>* allTx{nullptr};
    int consumed{0}; // index into allTx already turned into text
    int peerNs{0};
    int pmsRx{0};    // count of in-sequence I-frames received from the mailbox

    int pmsVs() const
    {
        int n = 0;
        for (const QByteArray& f : *allTx) {
            auto d = Frame::decode(f);
            if (d && d->type == FrameType::I && d->dest == peer)
                ++n;
        }
        return n % 8;
    }

    // New text the mailbox has sent since the last call. Acknowledges each
    // mailbox I-frame with an RR and loops, so a multi-frame reply fully drains
    // under the half-duplex window=1 (one frame per ack) exactly as a real TNC
    // would — without this, only the first I-frame of each reply would ever be
    // emitted.
    QString drainText()
    {
        QByteArray t;
        for (;;) {
            int newFrames = 0;
            for (; consumed < allTx->size(); ++consumed) {
                auto d = Frame::decode(allTx->at(consumed));
                if (d && d->type == FrameType::I && d->dest == peer) {
                    t += d->info;
                    ++pmsRx;
                    ++newFrames;
                }
            }
            if (newFrames == 0)
                break;
            // Ack everything received so far; this opens the mailbox's send
            // window so it emits the next I-frame (collected on the next pass).
            pms->onAirFrame(
                Frame::makeS(local, peer, FrameType::RR, pmsRx % 8, false, false).encode());
        }
        return QString::fromLatin1(t);
    }

    void send(const QByteArray& line)
    {
        pms->onAirFrame(
            Frame::makeI(local, peer, peerNs, pmsVs(), true, line).encode());
        peerNs = (peerNs + 1) % 8;
    }
};
} // namespace

static void testMailbox()
{
    PmsMailbox pms;
    pms.setVersionString(QStringLiteral("test"));
    QVector<QByteArray> tx;
    QObject::connect(&pms, &PmsMailbox::transmitFrame,
                     [&](const QByteArray& f) { tx.append(f); });

    pms.setListenCallsign(QStringLiteral("N0PMS-1"));
    pms.setAliasCallsign(QStringLiteral("AETBBS")); // AX.25 callsigns are <= 6 chars
    pms.setEnabled(true);

    const Address local{QStringLiteral("N0PMS"), 1, false, false};
    const Address peer{QStringLiteral("K7ABC"), 0, false, false};
    Peer p{&pms, local, peer, &tx, 0, 0};

    // Connect.
    pms.onAirFrame(Frame::makeU(local, peer, FrameType::SABM, true, true).encode());
    bool sawUA = false;
    for (const QByteArray& f : tx) {
        auto d = Frame::decode(f);
        if (d && d->type == FrameType::UA)
            sawUA = true;
    }
    CHECK(sawUA, "mailbox UA on connect");
    const QString greeting = p.drainText();
    CHECK(greeting.contains(QLatin1String("AetherMailbox")), "greeting names AetherMailbox");
    CHECK(greeting.contains(QLatin1String("K7ABC")), "greeting names the caller");
    CHECK(greeting.contains(QLatin1String("ENTER COMMAND")), "greeting shows prompt");

    // Help.
    p.send(QByteArray("H\r"));
    const QString help = p.drainText();
    CHECK(help.contains(QLatin1String("B(ye)")), "help lists commands");

    // Compose a private message to W1XYZ.
    p.send(QByteArray("SP W1XYZ\r"));
    CHECK(p.drainText().contains(QLatin1String("SUBJECT")), "send prompts for subject");
    p.send(QByteArray("Test subject\r"));
    CHECK(p.drainText().contains(QLatin1String("ENTER MESSAGE")), "prompts for body");
    p.send(QByteArray("Line one of body\r"));
    p.send(QByteArray("/EX\r"));
    CHECK(p.drainText().contains(QLatin1String("SAVED")), "message saved");
    CHECK(pms.messageCount() == 1, "one message stored");

    // List + read.
    p.send(QByteArray("L\r"));
    const QString list = p.drainText();
    CHECK(list.contains(QLatin1String("W1XYZ")), "list shows recipient");
    p.send(QByteArray("R 1\r"));
    const QString read = p.drainText();
    CHECK(read.contains(QLatin1String("Test subject")), "read shows subject");
    CHECK(read.contains(QLatin1String("Line one of body")), "read shows body");

    // Heard list should include the caller (we received their frames).
    p.send(QByteArray("J\r"));
    const QString jheard = p.drainText();
    CHECK(jheard.contains(QLatin1String("K7ABC")), "jheard lists the caller");

    // Bye.
    p.send(QByteArray("B\r"));
    bool sawDisc = false;
    for (const QByteArray& f : tx) {
        auto d = Frame::decode(f);
        if (d && d->type == FrameType::DISC)
            sawDisc = true;
    }
    CHECK(sawDisc, "mailbox sends DISC on BYE");
}

// A caller dialing the vanity alias should connect, and every reply (incl. the
// UA and greeting) must come from the alias address, not the primary.
static void testAliasDial()
{
    PmsMailbox pms;
    pms.setVersionString(QStringLiteral("test"));
    QVector<QByteArray> tx;
    QObject::connect(&pms, &PmsMailbox::transmitFrame,
                     [&](const QByteArray& f) { tx.append(f); });

    pms.setListenCallsign(QStringLiteral("N0PMS-1"));
    pms.setAliasCallsign(QStringLiteral("AETBBS")); // AX.25 callsigns are <= 6 chars
    pms.setEnabled(true);

    const Address alias{QStringLiteral("AETBBS"), 0, false, false};
    const Address peer{QStringLiteral("K7ABC"), 0, false, false};

    pms.onAirFrame(Frame::makeU(alias, peer, FrameType::SABM, true, true).encode());

    bool sawAliasUA = false;
    bool greetingFromAlias = false;
    for (const QByteArray& f : tx) {
        auto d = Frame::decode(f);
        if (!d || d->dest != peer)
            continue;
        if (d->type == FrameType::UA && d->src == alias)
            sawAliasUA = true;
        if (d->type == FrameType::I && d->src == alias)
            greetingFromAlias = true;
    }
    CHECK(sawAliasUA, "alias dial: UA answered from the alias address");
    CHECK(greetingFromAlias, "alias dial: greeting sent from the alias address");
    CHECK(pms.connectedCaller() == QLatin1String("K7ABC"), "alias dial: caller connected");

    // A frame to the primary while idle would also be accepted, but a frame to a
    // third, unrelated callsign must be ignored.
    const Address other{QStringLiteral("N9ZZZ"), 5, false, false};
    const int txBefore = tx.size();
    pms.onAirFrame(Frame::makeU(other, peer, FrameType::SABM, true, true).encode());
    CHECK(tx.size() == txBefore, "frames to an unrelated callsign are ignored");
}

int main(int argc, char** argv)
{
    // Isolate AppSettings / PMS storage from the real user config so the test is
    // repeatable and never touches a live operator's mailbox. AppSettings derives
    // its path from the home/config location, so redirect those before the
    // singleton is first used.
    TestSettingsProfile settingsProfile(QStringLiteral("aether-pms-mailbox-test"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    // PmsMailbox honours AETHER_PMS_DIR for its JSON store; point it at the clean
    // temp dir so the test is repeatable and never touches a real mailbox.
    qputenv("AETHER_PMS_DIR",
            (settingsProfile.path() + QStringLiteral("/pms")).toUtf8());

    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    testAddress();
    testFrameRoundTrip();
    testConnection();
    testHalfDuplexWindowOneDrainsMultiFrame();
    testMailbox();
    testAliasDial();

    if (g_failures == 0) {
        std::printf("All PMS mailbox tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "%d PMS mailbox test(s) failed.\n", g_failures);
    return 1;
}
