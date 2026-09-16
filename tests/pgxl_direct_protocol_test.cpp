// The PGXL's direct port-9008 protocol — PgxlConnection's parser and the
// per-port block AmpModel builds from it, driven over a real socket against a
// stub amplifier on loopback, the same shape as tgxl_direct_protocol_test.
//
// Every status frame below is verbatim from a read-only capture of a
// PowerGeniusXL on firmware 3.8.9 (serial 10-200/24-0203), taken by sending
// the two commands the client already sends on connect — `info` and `status`
// — and reading what came back. Nothing in that capture commanded the
// amplifier.
//
// What the per-port block answers that the radio-relayed "amplifier" object
// cannot: which band each RF port is on, which bias profile it is configured
// for, and which radio feeds it. The relayed object carries model, serial,
// ip, state and the antenna map, and nothing else — so without this the
// applet's port strips would have nothing to show.

#include "core/PgxlConnection.h"
#include "models/AmpModel.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include <cstdio>
#include <functional>

using namespace AetherSDR;

namespace {

int g_failures = 0;

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

bool spin(std::function<bool()> done, int timeoutMs = 5000)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!done() && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

// The status reply, verbatim, with the state word substituted. Everything
// else — the bands, the bias profiles, the FLEX-8600 on both ports — is
// exactly what the amplifier sent.
QByteArray statusReply(const char* seq, const char* state)
{
    return QByteArray("R") + seq + "|0|state=" + state +
        " bandA=40 bandB=0 bsrcA=FLEX bsrcB=FLEX bsrcAutoA=0 bsrcAutoB=0"
        " flexA=FLEX-8600 flexB=FLEX-8600 vac=245 vdd=0.0 id=0.0 peakid=0.0"
        " fwd=30.0 peakfwd=30.0 swr=-60.0 temp=23.1 cntfreq=0 cat1freq=0"
        " cat2freq=0 hltemp=23.4 biasA=RADIO_AAB biasB=RADIO_AB"
        " fanmode=STANDARD meffa=STANDBY\n";
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        // A sandbox that cannot bind loopback has not found a defect; skip.
        // tests.cmake maps 77 to SKIP.
        std::fprintf(stderr, "No loopback bind available — skipping\n");
        return 77;
    }

    PgxlConnection conn;
    QSignalSpy connected(&conn, &PgxlConnection::connected);
    QSignalSpy alerts(&conn, &PgxlConnection::alertChanged);
    CHECK(alerts.isValid());

    AmpModel model;
    model.setDirectConnection(&conn);
    QSignalSpy ports(&model, &AmpModel::portsChanged);
    QSignalSpy states(&model, &AmpModel::ampStateChanged);

    conn.connectToPgxl(QStringLiteral("127.0.0.1"), server.serverPort());
    CHECK(spin([&] { return server.hasPendingConnections(); }));

    QTcpSocket* peer = server.nextPendingConnection();
    CHECK(peer != nullptr);
    if (!peer) return 1;

    // The amplifier greets with its version; the client only treats the
    // session as live after that.
    peer->write("V3.8.9\n");
    peer->flush();
    CHECK(spin([&] { return connected.count() == 1; }));
    CHECK(conn.version() == QLatin1String("3.8.9"));
    CHECK(model.hasDirectConnection());

    // The `info` reply, verbatim — note the double space the amplifier sends
    // after the serial, which a key/value split must tolerate rather than
    // turn into an empty key. It carries no per-port block, so it must not
    // be mistaken for one and publish empty ports.
    peer->write("R1|0|serial=10-200/24-0203  version=3.8.9 protocol=1.0 mains=240\n");
    peer->flush();
    spin([&] { return ports.count() > 0; }, 300);
    CHECK(ports.count() == 0);
    CHECK(!model.hasPortInfo());

    // ── The per-port block ────────────────────────────────────────────
    peer->write(statusReply("2", "IDLE"));
    peer->flush();
    CHECK(spin([&] { return ports.count() >= 1; }));
    CHECK(model.hasPortInfo());
    CHECK(model.stateText() == QLatin1String("IDLE"));

    // Port A is on 40m. The band is the live reading — it is what says the
    // amplifier has something on the port at all.
    CHECK(model.portA().live);
    CHECK(model.portA().band == QLatin1String("40"));
    CHECK(model.portA().source == QLatin1String("FLEX-8600"));
    // The RADIO_ prefix says where the bias choice comes from and reads the
    // same on every port, so only the profile is kept.
    CHECK(model.portA().bias == QLatin1String("AAB"));
    CHECK(!model.portA().ptt);

    // Port B: bandB=0 is how the amplifier reports a port nothing is driving.
    // It is NOT a band named "0", and the port is not live — while flexB
    // still reads FLEX-8600, exactly as flexB does on the tuner. A source
    // name is configuration, not evidence that RF is flowing.
    CHECK(!model.portB().live);
    CHECK(model.portB().band.isEmpty());
    CHECK(model.portB().bias == QLatin1String("AB"));
    CHECK(model.portB().source == QLatin1String("FLEX-8600"));

    // An unchanged status does not re-announce: the client polls at 5 Hz and
    // a repaint per poll is a repaint per poll forever.
    {
        const int settled = ports.count();
        peer->write(statusReply("3", "IDLE"));
        peer->flush();
        spin([&] { return ports.count() > settled; }, 300);
        CHECK(ports.count() == settled);
    }

    // ── Keying ────────────────────────────────────────────────────────
    //
    // The PGXL carries no per-port PTT field. Which port is keyed is in the
    // state word itself — TRANSMIT_A / TRANSMIT_B — so the lamps are derived
    // from it, and exactly one of them can be lit.
    {
        const int settled = ports.count();
        peer->write(statusReply("4", "TRANSMIT_A"));
        peer->flush();
        CHECK(spin([&] { return ports.count() > settled; }));
        CHECK(model.stateText() == QLatin1String("TRANSMIT_A"));
        CHECK(model.portA().ptt);
        CHECK(!model.portB().ptt);
    }
    {
        peer->write(statusReply("5", "TRANSMIT_B"));
        peer->flush();
        CHECK(spin([&] { return model.portB().ptt; }));
        CHECK(!model.portA().ptt);
    }
    {
        peer->write(statusReply("6", "IDLE"));
        peer->flush();
        CHECK(spin([&] { return !model.portB().ptt; }));
        CHECK(!model.portA().ptt);
    }

    // A state push with a prefix word before the first key parses the same
    // way a status reply does — the amplifier pushes unsolicited status in
    // that shape, and the keying lamps must follow it.
    {
        peer->write("S0|TRANSMIT_A id=39 vac=241 vdd=51.9 state=TRANSMIT_A\n");
        peer->flush();
        CHECK(spin([&] { return model.stateText() == QLatin1String("TRANSMIT_A"); }));
        CHECK(model.portA().ptt);
        CHECK(!model.portB().ptt);
    }

    // ── The alert channel ─────────────────────────────────────────────
    //
    // `M|<text>`, cleared by an empty body. No PGXL alert was captured — the
    // amplifier had nothing to complain about — so what this pins is the
    // parser, not a recorded device behaviour. The frame shape is the
    // tuner's, which is the same vendor's protocol on the same C/R/S/V
    // framing, and handling it costs nothing if the amplifier never sends
    // one.
    peer->write("M|PA OVERTEMP\n");
    peer->flush();
    CHECK(spin([&] { return alerts.count() == 1; }));
    CHECK(model.alert() == QLatin1String("PA OVERTEMP"));

    peer->write("M|\n");
    peer->flush();
    CHECK(spin([&] { return model.alert().isEmpty(); }));

    // A response frame is not an alert, even though both begin with a letter
    // and a pipe. R carries a sequence number; M never does.
    peer->write("M|PA OVERTEMP\n");
    peer->flush();
    CHECK(spin([&] { return !model.alert().isEmpty(); }));
    {
        const int settled = alerts.count();
        peer->write("R42|0|\n");
        peer->flush();
        spin([&] { return alerts.count() > settled; }, 300);
        CHECK(alerts.count() == settled);
        CHECK(model.alert() == QLatin1String("PA OVERTEMP"));
    }

    // Losing the amplifier drops the readings rather than freezing them. A
    // band or a bias left standing claims the amplifier is set up a way we
    // have stopped being told about, and a fault banner that outlives the
    // connection cannot be cleared by the device that raised it.
    peer->close();
    CHECK(spin([&] { return !model.hasPortInfo(); }));
    CHECK(!model.portA().live);
    CHECK(model.portA().band.isEmpty());
    CHECK(model.alert().isEmpty());
    CHECK(!model.hasDirectConnection());

    conn.disconnect();

    if (g_failures == 0) {
        std::printf("pgxl_direct_protocol_test: all checks passed\n");
        return 0;
    }
    std::printf("pgxl_direct_protocol_test: %d failure(s)\n", g_failures);
    return 1;
}
