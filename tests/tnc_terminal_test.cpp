// Integration tests for the TNC terminal (connected-mode AX.25 *client*). These
// drive a TncTerminal against a bare Ax25Connection acting as the answering BBS,
// cross-wiring their frames over a simulated half-duplex air channel. Everything
// runs synchronously (nested signal emission) — no DSP, radio, or event loop —
// exactly mirroring how pms_mailbox_test exercises the answering side.

#include "core/tnc/Ax25.h"
#include "core/tnc/Ax25Connection.h"
#include "core/tnc/HeardList.h"
#include "core/tnc/TncTerminal.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QObject>
#include <QString>
#include <QStringList>

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

namespace {
Address call(const char* text)
{
    auto a = Address::parse(QString::fromLatin1(text));
    return a ? *a : Address{};
}
} // namespace

// Full happy-path session: connect, exchange data both ways, escape to command
// mode without dropping the link, then BYE to disconnect.
static void testConnectConverseDisconnect()
{
    TncTerminal client;
    client.setMyCall(QStringLiteral("N0AAA"));

    Ax25Connection bbs; // the answering side (a BBS)
    bbs.setLocalAddress(call("N0BBB-1"));

    // Simulated air: each side's outbound frames are the other's inbound frames.
    QObject::connect(&client, &TncTerminal::transmitFrame, [&](const QByteArray& raw) {
        if (auto f = Frame::decode(raw))
            bbs.onFrameReceived(*f);
    });
    QObject::connect(&bbs, &Ax25Connection::sendFrame, [&](const QByteArray& raw) {
        client.onAirFrame(raw);
    });

    QStringList out;
    QObject::connect(&client, &TncTerminal::output, [&](const QString& t) { out += t; });
    QByteArray bbsRx;
    QObject::connect(&bbs, &Ax25Connection::dataReceived,
                     [&](const QByteArray& d) { bbsRx += d; });
    bool bbsConnected = false;
    bool bbsDisconnected = false;
    QObject::connect(&bbs, &Ax25Connection::connected,
                     [&](const Address&) { bbsConnected = true; });
    QObject::connect(&bbs, &Ax25Connection::disconnected,
                     [&](const Address&, bool) { bbsDisconnected = true; });

    // --- CONNECT --------------------------------------------------------------
    client.submitLine(QStringLiteral("C N0BBB-1"));
    CHECK(bbsConnected, "BBS accepted the connect (SABM/UA handshake)");
    CHECK(client.isConnected(), "client reports connected");
    CHECK(client.mode() == TncTerminal::Mode::Converse, "client entered converse mode");
    CHECK(out.join(QString()).contains(QLatin1String("CONNECTED")),
          "transcript shows CONNECTED");

    // --- RX from the BBS ------------------------------------------------------
    bbs.sendData(QByteArray("Welcome to N0BBB BBS\r"));
    const QString joined = out.join(QString());
    CHECK(joined.contains(QLatin1String("Welcome to N0BBB BBS")), "BBS text shown");
    CHECK(!joined.contains(QLatin1Char('\r')), "CR stripped from displayed text");

    // --- TX to the BBS (converse mode) ---------------------------------------
    client.submitLine(QStringLiteral("LIST"));
    CHECK(bbsRx == QByteArray("LIST\r"), "converse line sent CR-terminated to BBS");

    // --- escape returns to command mode WITHOUT disconnecting -----------------
    client.submitLine(QStringLiteral("~"));
    CHECK(client.mode() == TncTerminal::Mode::Command, "escape returned to command mode");
    CHECK(client.isConnected(), "link still up after escape");

    // --- BYE disconnects ------------------------------------------------------
    client.submitLine(QStringLiteral("BYE"));
    CHECK(bbsDisconnected, "BBS saw the DISC");
    CHECK(!client.isConnected(), "client link is down after BYE");
}

// A busy/unknown peer replies DM — the connect must fail cleanly and land back
// in command mode (no half-open link).
static void testConnectRefusedDm()
{
    TncTerminal client;
    client.setMyCall(QStringLiteral("N0AAA"));

    // Responder: answer any SABM with a DM (refuse).
    QObject::connect(&client, &TncTerminal::transmitFrame, [&](const QByteArray& raw) {
        auto f = Frame::decode(raw);
        if (f && f->type == FrameType::SABM) {
            Frame dm = Frame::makeU(f->src, f->dest, FrameType::DM,
                                    f->pollFinal, /*command=*/false);
            client.onAirFrame(dm.encode());
        }
    });

    QStringList out;
    QObject::connect(&client, &TncTerminal::output, [&](const QString& t) { out += t; });

    client.submitLine(QStringLiteral("C N0BBB"));
    CHECK(!client.isConnected(), "refused connect leaves us disconnected");
    CHECK(!client.isConnecting(), "no lingering connecting state");
    CHECK(client.mode() == TncTerminal::Mode::Command, "back in command mode after refusal");
    CHECK(out.join(QString()).contains(QLatin1String("FAILED")),
          "transcript reports the connect failure");
}

// Guard rails: command parsing without a link.
static void testCommandGuards()
{
    TncTerminal client;
    QStringList out;
    QObject::connect(&client, &TncTerminal::output, [&](const QString& t) { out += t; });

    // CONNECT before MYCALL is set must be rejected.
    client.setMyCall(QString());
    client.submitLine(QStringLiteral("CONNECT N0BBB"));
    CHECK(!client.isConnecting(), "CONNECT without MYCALL does not dial");
    CHECK(out.join(QString()).contains(QLatin1String("MYCALL")),
          "prompted to set MYCALL");

    // MYCALL command sets the address.
    out.clear();
    client.submitLine(QStringLiteral("MYCALL N0AAA-5"));
    CHECK(client.myCall() == QLatin1String("N0AAA-5"), "MYCALL command set the call");

    // Unknown command is reported, not silently ignored.
    out.clear();
    client.submitLine(QStringLiteral("FLOOB"));
    CHECK(out.join(QString()).contains(QLatin1String("unknown")),
          "unknown command reported");

    // BYE while idle is a no-op that says so.
    out.clear();
    client.submitLine(QStringLiteral("BYE"));
    CHECK(out.join(QString()).contains(QLatin1String("Not connected")),
          "BYE while idle reports not connected");
}

// CONNECT ... VIA must attach the digipeater path to the outbound SABM so the
// digi repeats us to the peer.
static void testConnectViaDigipeater()
{
    TncTerminal client;
    client.setMyCall(QStringLiteral("N0AAA"));

    QByteArray firstFrame;
    QObject::connect(&client, &TncTerminal::transmitFrame, [&](const QByteArray& raw) {
        if (firstFrame.isEmpty())
            firstFrame = raw;
    });

    client.submitLine(QStringLiteral("C KX9X-2 VIA WIDE1-1,RELAY"));
    auto f = Frame::decode(firstFrame);
    CHECK(f && f->type == FrameType::SABM, "outbound SABM emitted");
    CHECK(f && f->dest == call("KX9X-2"), "SABM addressed to the peer");
    CHECK(f && f->via.size() == 2, "two digipeaters in the path");
    CHECK(f && f->via.size() == 2 && f->via.at(0) == call("WIDE1-1")
              && f->via.at(1) == call("RELAY"), "digi path in order");
    CHECK(f && !f->via.isEmpty() && !f->via.at(0).hasBeenRepeated,
          "outbound digis not yet repeated (H=0)");
}

// MHEARD lists stations from the shared heard log, including the last UI beacon.
static void testMheard()
{
    HeardList heard;
    // A plain frame and a UI beacon from two stations.
    heard.record(Frame::makeI(call("N0AAA"), call("W1ABC"), 0, 0, false, QByteArray("hi")));
    heard.record(Frame::makeUI(call("BEACON"), call("K7XYZ-1"), {},
                               QByteArray("AetherMailbox online - connect for mail")));

    TncTerminal client;
    client.setHeardList(&heard);

    QStringList out;
    QObject::connect(&client, &TncTerminal::output, [&](const QString& t) { out += t; });

    client.submitLine(QStringLiteral("MHEARD"));
    const QString joined = out.join(QString());
    CHECK(joined.contains(QLatin1String("W1ABC")), "MHEARD lists a heard station");
    CHECK(joined.contains(QLatin1String("K7XYZ-1")), "MHEARD shows SSID");
    CHECK(joined.contains(QLatin1String("AetherMailbox online")),
          "MHEARD shows the last beacon text");
}

// Lost-UA recovery: we send SABM, the peer accepts but its UA is lost on the
// air, and the peer's prompt I-frame arrives while we still think we're
// connecting. We must adopt the link, deliver the data, and ack — not ignore it
// or reply DM. (Regression for the live 2026-06-03 W6RAY-3 session: connect went
// live but no data ever transferred.)
static void testLostUaAdoptionOnIFrame()
{
    Ax25Connection conn;
    conn.setLocalAddress(call("N0AAA"));

    QVector<QByteArray> tx;
    bool connected = false;
    QByteArray rx;
    QObject::connect(&conn, &Ax25Connection::sendFrame,
                     [&](const QByteArray& f) { tx.append(f); });
    QObject::connect(&conn, &Ax25Connection::connected,
                     [&](const Address&) { connected = true; });
    QObject::connect(&conn, &Ax25Connection::dataReceived,
                     [&](const QByteArray& d) { rx = d; });

    conn.connectTo(call("N0BBB"));
    CHECK(!connected, "not connected until the peer responds");

    // No UA arrives; the peer's prompt I-frame (NS=0) shows up instead.
    conn.onFrameReceived(Frame::makeI(call("N0AAA"), call("N0BBB"), 0, 0, true,
                                      QByteArray("Welcome to N0BBB\r")));
    CHECK(connected, "adopted the link on the peer's I-frame (lost UA)");
    CHECK(conn.isConnected(), "state is Connected after adoption");
    CHECK(rx == QByteArray("Welcome to N0BBB\r"), "prompt data delivered, not DM'd");
    bool sawRR = false;
    bool sawDm = false;
    for (const QByteArray& f : tx) {
        auto d = Frame::decode(f);
        if (d && d->type == FrameType::RR && d->nr == 1)
            sawRR = true;
        if (d && d->type == FrameType::DM)
            sawDm = true;
    }
    CHECK(sawRR, "acked the adopted I-frame with RR NR=1");
    CHECK(!sawDm, "never sent DM in response to the prompt");
}

// Same recovery, but the peer's first post-connect frame is an RR poll (as seen
// live). We must adopt and answer the poll with an RR final, not keep SABMing.
static void testLostUaAdoptionOnRrPoll()
{
    Ax25Connection conn;
    conn.setLocalAddress(call("N0AAA"));

    QVector<QByteArray> tx;
    bool connected = false;
    QObject::connect(&conn, &Ax25Connection::sendFrame,
                     [&](const QByteArray& f) { tx.append(f); });
    QObject::connect(&conn, &Ax25Connection::connected,
                     [&](const Address&) { connected = true; });

    conn.connectTo(call("N0BBB"));
    tx.clear(); // drop the SABM; we care about the response to the poll

    // Peer polls us with RR command, P=1 (it already thinks we're connected).
    conn.onFrameReceived(Frame::makeS(call("N0AAA"), call("N0BBB"), FrameType::RR,
                                      0, /*pf=*/true, /*cmd=*/true));
    CHECK(connected, "adopted the link on the peer's RR poll");
    bool sawRrFinal = false;
    bool sawSabm = false;
    for (const QByteArray& f : tx) {
        auto d = Frame::decode(f);
        if (d && d->type == FrameType::RR && d->pollFinal)
            sawRrFinal = true;
        if (d && d->type == FrameType::SABM)
            sawSabm = true;
    }
    CHECK(sawRrFinal, "answered the RR poll with an RR final");
    CHECK(!sawSabm, "did not retransmit SABM after adopting");
}

// Multi-frame reply (e.g. a long BBS help menu): the peer sends a window of
// I-frames in one burst. On a half-duplex link we must NOT key up to ack each
// unpolled frame mid-burst (that would deafen us to the rest of the window and
// stall the menu). We ack once — when the final, polled frame arrives.
// (Regression for the reported "first packet shows, then stalls".)
static void testMultiFrameDeferredAck()
{
    Ax25Connection conn;
    conn.setLocalAddress(call("N0AAA"));

    QVector<QByteArray> tx;
    QByteArray rx;
    QObject::connect(&conn, &Ax25Connection::sendFrame,
                     [&](const QByteArray& f) { tx.append(f); });
    QObject::connect(&conn, &Ax25Connection::dataReceived,
                     [&](const QByteArray& d) { rx += d; });

    // Establish the link (client connectTo + UA).
    conn.connectTo(call("N0BBB"));
    conn.onFrameReceived(Frame::makeU(call("N0AAA"), call("N0BBB"), FrameType::UA,
                                      /*pf=*/true, /*cmd=*/false));
    tx.clear();

    auto countType = [&](FrameType t) {
        int n = 0;
        for (const QByteArray& f : tx) {
            auto d = Frame::decode(f);
            if (d && d->type == t)
                n++;
        }
        return n;
    };

    // Three-frame menu; only the last frame polls (P=1).
    conn.onFrameReceived(Frame::makeI(call("N0AAA"), call("N0BBB"), 0, 0, /*pf=*/false,
                                      QByteArray("MENU line 1\r")));
    conn.onFrameReceived(Frame::makeI(call("N0AAA"), call("N0BBB"), 1, 0, /*pf=*/false,
                                      QByteArray("MENU line 2\r")));
    CHECK(countType(FrameType::RR) == 0, "no ack keyed mid-burst (would deafen us)");

    conn.onFrameReceived(Frame::makeI(call("N0AAA"), call("N0BBB"), 2, 0, /*pf=*/true,
                                      QByteArray("MENU line 3\r")));
    CHECK(rx == QByteArray("MENU line 1\rMENU line 2\rMENU line 3\r"),
          "all three menu frames delivered in order");
    CHECK(countType(FrameType::RR) == 1, "exactly one coalesced ack for the burst");

    bool finalAckOk = false;
    for (const QByteArray& f : tx) {
        auto d = Frame::decode(f);
        if (d && d->type == FrameType::RR && d->nr == 3 && d->pollFinal)
            finalAckOk = true;
    }
    CHECK(finalAckOk, "ack is RR NR=3 with F=1 (acks the whole window)");
}

// Root cause of the "stall while entering commands, far end keeps retrying, then
// disconnects us" report: when the peer REJ's an outbound I-frame, we must resend
// it from our store. The old code rewound V(S) and called pumpOutbound(), but the
// frame's payload was already consumed from the send buffer, so nothing was
// resent and the link silently desynced.
static void testRejTriggersResend()
{
    Ax25Connection conn;
    conn.setLocalAddress(call("N0AAA"));

    QVector<QByteArray> tx;
    QObject::connect(&conn, &Ax25Connection::sendFrame,
                     [&](const QByteArray& f) { tx.append(f); });

    conn.connectTo(call("N0BBB"));
    conn.onFrameReceived(Frame::makeU(call("N0AAA"), call("N0BBB"), FrameType::UA,
                                      /*pf=*/true, /*cmd=*/false));
    tx.clear();

    auto countINs = [&](int ns) {
        int n = 0;
        for (const QByteArray& f : tx) {
            auto d = Frame::decode(f);
            if (d && d->type == FrameType::I && d->ns == ns)
                n++;
        }
        return n;
    };

    conn.sendData(QByteArray("HELP\r"));
    CHECK(countINs(0) == 1, "command goes out as I-frame N(S)=0");

    // Peer rejects, asking us to resend starting at N(S)=0.
    conn.onFrameReceived(Frame::makeS(call("N0AAA"), call("N0BBB"), FrameType::REJ,
                                      /*nr=*/0, /*pf=*/true, /*cmd=*/true));
    CHECK(countINs(0) == 2, "REJ N(R)=0 retransmits the outstanding I-frame N(S)=0");
}

// On a half-duplex window=1 link, each outbound I-frame must poll (P=1) so the
// peer acks immediately instead of batching the ack and stalling us into a T1
// retransmit.
static void testOutboundFramePolls()
{
    Ax25Connection conn;
    conn.setLocalAddress(call("N0AAA"));

    QVector<QByteArray> tx;
    QObject::connect(&conn, &Ax25Connection::sendFrame,
                     [&](const QByteArray& f) { tx.append(f); });

    conn.connectTo(call("N0BBB"));
    conn.onFrameReceived(Frame::makeU(call("N0AAA"), call("N0BBB"), FrameType::UA,
                                      /*pf=*/true, /*cmd=*/false));
    tx.clear();

    conn.sendData(QByteArray("X\r"));
    bool polled = false;
    for (const QByteArray& f : tx) {
        auto d = Frame::decode(f);
        if (d && d->type == FrameType::I && d->ns == 0)
            polled = d->pollFinal;
    }
    CHECK(polled, "single outbound I-frame sets P=1 (window=1)");
}

// Reject-exception: a run of out-of-sequence I-frames must produce exactly ONE
// REJ, not one per frame. The repeated-REJ storm was keeping us keyed on a
// half-duplex link and deaf to the retransmission we asked for, stalling the
// session (observed live with SJVBBS-1: missed NS=1, then a REJ-per-NS=2 storm).
static void testRejectExceptionSuppressesStorm()
{
    Ax25Connection conn;
    conn.setLocalAddress(call("N0AAA"));

    QVector<QByteArray> tx;
    QObject::connect(&conn, &Ax25Connection::sendFrame,
                     [&](const QByteArray& f) { tx.append(f); });

    conn.connectTo(call("N0BBB"));
    conn.onFrameReceived(Frame::makeU(call("N0AAA"), call("N0BBB"), FrameType::UA,
                                      /*pf=*/true, /*cmd=*/false));
    tx.clear();

    auto countREJ = [&] {
        int n = 0;
        for (const QByteArray& f : tx) {
            auto d = Frame::decode(f);
            if (d && d->type == FrameType::REJ)
                n++;
        }
        return n;
    };

    // Expected N(S)=0, but a run of POLLED NS=2 retransmits arrives — exactly
    // the SJVBBS-1 pattern. We must still REJ only once and then stay silent;
    // answering each poll is what created the half-duplex phase-lock.
    for (int i = 0; i < 4; ++i)
        conn.onFrameReceived(Frame::makeI(call("N0AAA"), call("N0BBB"), 2, 0,
                                          /*pf=*/true, QByteArray("x")));
    CHECK(countREJ() == 1, "a run of polled out-of-sequence frames yields exactly one REJ");

    // The awaited in-sequence frame clears the reject exception...
    conn.onFrameReceived(Frame::makeI(call("N0AAA"), call("N0BBB"), 0, 0,
                                      /*pf=*/false, QByteArray("hi")));
    // ...so a fresh gap is allowed to REJ again.
    conn.onFrameReceived(Frame::makeI(call("N0AAA"), call("N0BBB"), 3, 0,
                                      /*pf=*/false, QByteArray("y")));
    CHECK(countREJ() == 2, "a new gap after recovery REJs again");
}

// An N(R) that acknowledges frames we never sent must be ignored, not applied —
// applying it walks V(A) past V(S) around the mod-8 ring and corrupts the send
// window (the harness caught this as a phantom outstanding count).
static void testInvalidNrIgnored()
{
    Ax25Connection conn;
    conn.setLocalAddress(call("N0AAA"));

    conn.connectTo(call("N0BBB"));
    conn.onFrameReceived(Frame::makeU(call("N0AAA"), call("N0BBB"), FrameType::UA,
                                      /*pf=*/true, /*cmd=*/false));
    // We have sent no I-frames: V(S)=V(A)=0, nothing outstanding.
    conn.onFrameReceived(Frame::makeS(call("N0AAA"), call("N0BBB"), FrameType::RR,
                                      /*nr=*/3, /*pf=*/false, /*cmd=*/false));
    CHECK(conn.sendSeq() == 0, "V(S) unchanged by an out-of-range N(R)");
    CHECK(conn.unacked() == 0, "send window intact (no phantom outstanding frames)");
}

// CONV (back to converse) and STATUS (connection stats) command-mode commands.
static void testConvAndStatusCommands()
{
    TncTerminal client;
    client.setMyCall(QStringLiteral("N0AAA"));

    Ax25Connection bbs;
    bbs.setLocalAddress(call("N0BBB"));
    QObject::connect(&client, &TncTerminal::transmitFrame, [&](const QByteArray& raw) {
        if (auto f = Frame::decode(raw)) bbs.onFrameReceived(*f);
    });
    QObject::connect(&bbs, &Ax25Connection::sendFrame, [&](const QByteArray& raw) {
        client.onAirFrame(raw);
    });

    QStringList out;
    QObject::connect(&client, &TncTerminal::output, [&](const QString& t) { out += t; });

    // CONV before connecting is rejected.
    client.submitLine(QStringLiteral("CONV"));
    CHECK(out.join(QString()).contains(QLatin1String("Not connected")),
          "CONV while idle reports not connected");

    // STATUS works any time and shows the key fields.
    out.clear();
    client.submitLine(QStringLiteral("STATUS"));
    const QString st = out.join(QString());
    CHECK(st.contains(QLatin1String("STATUS")) && st.contains(QLatin1String("Packets"))
              && st.contains(QLatin1String("Retries")),
          "STATUS prints the stats block");
    CHECK(st.contains(QLatin1String("N0AAA")), "STATUS shows our callsign");

    // Connect, drop to command mode, then CONV returns to converse.
    client.submitLine(QStringLiteral("C N0BBB"));
    CHECK(client.isConnected(), "connected for CONV test");
    client.submitLine(QStringLiteral("~")); // escape to command mode
    CHECK(client.mode() == TncTerminal::Mode::Command, "escaped to command mode");
    out.clear();
    client.submitLine(QStringLiteral("CONV"));
    CHECK(client.mode() == TncTerminal::Mode::Converse, "CONV returns to converse mode");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testConvAndStatusCommands();
    testConnectConverseDisconnect();
    testConnectRefusedDm();
    testCommandGuards();
    testConnectViaDigipeater();
    testMheard();
    testLostUaAdoptionOnIFrame();
    testLostUaAdoptionOnRrPoll();
    testMultiFrameDeferredAck();
    testRejTriggersResend();
    testOutboundFramePolls();
    testRejectExceptionSuppressesStorm();
    testInvalidNrIgnored();

    if (g_failures == 0)
        std::fprintf(stderr, "tnc_terminal_test: all checks passed\n");
    else
        std::fprintf(stderr, "tnc_terminal_test: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
