// The TGXL's direct port-9010 protocol — TgxlConnection's parser and the
// per-port block TunerModel builds from it, driven over a real socket against
// a stub tuner on loopback, the same shape as vkamp_connection_test.
//
// Two features, both reverse-engineered from a capture of the 4O3A
// TunerGeniusDesk application talking to a TGXL on firmware 1.2.17. Every
// frame below is verbatim from that capture.
//
// The tuner pushes an operator-facing alert when a tune cannot proceed, and
// clears it a few seconds later with an empty one. Both go to every connected
// client, not only the one that asked for the tune, so a client that merely
// watches still has to recognise them.
//
// Every frame below is verbatim from a capture of the 4O3A TunerGeniusDesk
// application talking to a TGXL on firmware 1.2.17: a tune started with ~5 W
// of drive raised "LOW RF POWER" ~40 ms in, and the tuner cleared it
// unprompted ~3 s later. `M` is its own frame type — it carries no sequence
// number, which is why it is matched before the R and S branches.

#include "core/TgxlConnection.h"
#include "models/TunerModel.h"

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

// Spins the event loop until done() or the deadline, returning done()'s last
// answer so callers can assert on it.
bool spin(std::function<bool()> done, int timeoutMs = 5000)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!done() && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        // A sandbox that cannot bind loopback has not found a defect; skip,
        // the way the sibling widget test skips without an accessibility
        // backend. tests.cmake maps 77 to SKIP.
        std::fprintf(stderr, "No loopback bind available — skipping\n");
        return 77;
    }

    TgxlConnection conn;
    QSignalSpy alerts(&conn, &TgxlConnection::alertChanged);
    QSignalSpy connected(&conn, &TgxlConnection::connected);
    CHECK(alerts.isValid());

    conn.connectToTgxl(QStringLiteral("127.0.0.1"), server.serverPort());
    CHECK(spin([&] { return server.hasPendingConnections(); }));

    QTcpSocket* peer = server.nextPendingConnection();
    CHECK(peer != nullptr);
    if (!peer) return 1;

    // The tuner greets with its version; the client only treats the session as
    // live after that, so no alert can be recognised before it.
    peer->write("V1.2.17\n");
    peer->flush();
    CHECK(spin([&] { return connected.count() == 1; }));
    CHECK(conn.version() == QLatin1String("1.2.17"));

    // The alert, exactly as captured.
    peer->write("M|LOW RF POWER\n");
    peer->flush();
    CHECK(spin([&] { return alerts.count() == 1; }));
    if (alerts.count() == 1) {
        CHECK(alerts.takeFirst().at(0).toString() == QLatin1String("LOW RF POWER"));
    }

    // The clear: an empty body, which the tuner sends on its own schedule.
    // It must read as "cleared", not as an alert whose text happens to be
    // blank — a banner that never goes away is worse than one that never
    // appears.
    peer->write("M|\n");
    peer->flush();
    CHECK(spin([&] { return alerts.count() == 1; }));
    if (alerts.count() == 1) {
        CHECK(alerts.takeFirst().at(0).toString().isEmpty());
    }

    // The completion notice: the tuner reports the solution it settled on,
    // ~50 ms after tuning clears. Verbatim from the capture, both cycles.
    peer->write("M|Tuned SWR: 1.13:1\n");
    peer->flush();
    CHECK(spin([&] { return alerts.count() == 1; }));
    if (alerts.count() == 1) {
        CHECK(alerts.takeFirst().at(0).toString() == QLatin1String("Tuned SWR: 1.13:1"));
    }

    // Its clear is the same empty frame a failure's clear uses -- the tuner
    // has one alert channel, not one per severity.
    peer->write("M|\n");
    peer->flush();
    CHECK(spin([&] { return alerts.count() == 1; }));
    if (alerts.count() == 1) {
        CHECK(alerts.takeFirst().at(0).toString().isEmpty());
    }

    // An alert arriving in the same TCP segment as other frames still parses:
    // the tuner coalesces freely, and the capture shows alerts landing
    // immediately beside state pushes.
    peer->write("S0|state state=1 tuning=0 bypass=0 relayC1=4 relayL=8 relayC2=4\n"
                "M|LOW RF POWER\n"
                "S0|state state=1 tuning=0 bypass=0 relayC1=0 relayL=0 relayC2=0\n");
    peer->flush();
    CHECK(spin([&] { return alerts.count() == 1; }));
    if (alerts.count() == 1) {
        CHECK(alerts.takeFirst().at(0).toString() == QLatin1String("LOW RF POWER"));
    }

    // A response frame is not an alert, even though both begin with a letter
    // and a pipe. R carries a sequence number; M never does.
    peer->write("R42|0|\n");
    peer->flush();
    spin([&] { return alerts.count() > 0; }, 300);
    CHECK(alerts.count() == 0);

    // ── The per-port block ────────────────────────────────────────────
    //
    // The status poll reports what each RF port is hearing. `modeX` is the
    // tuner's validity flag for the port: across the whole capture modeA=1
    // accompanied a real band and frequency every time (8494 samples) and
    // modeA=0 accompanied zeroes every time (26). It does NOT identify the
    // kind of source — note flexB below reads FLEX-8600 on a port carrying
    // nothing at all, which is exactly why a dead port must not be labelled
    // from it.
    TunerModel model;
    model.setDirectConnection(&conn);
    QSignalSpy ports(&model, &TunerModel::portsChanged);

    // `max` is a running maximum the tuner latches across transmissions, not a
    // rating: a live TGXL on firmware 1.2.17 was captured reporting max=0.00
    // on a freshly-opened session and 62.43 after a transmit. Nothing here
    // asserts on it; it is carried so the frame stays the shape the device
    // sends.
    peer->write("S230|status fwd=21.42 peak=21.42 max=62.43 swr=-60.0000 "
                "pttA=0 bandA=6 modeA=1 flexA=FLEX-8600 freqA=14161.500 "
                "bypassA=0 bypassRxA=0 antA=0 "
                "pttB=0 bandB=0 modeB=0 flexB=FLEX-8600 freqB=0.000 "
                "bypassB=0 bypassRxB=0 antB=0 "
                "state=1 active=1 tuning=0 bypass=0 ag=0 "
                "relayC1=44 relayL=12 relayC2=8\n");
    peer->flush();
    CHECK(spin([&] { return ports.count() >= 1; }));
    CHECK(model.hasPortInfo());

    CHECK(model.portA().live);
    CHECK(model.portA().source == QLatin1String("FLEX-8600"));
    // kHz on the wire, not MHz — 14161.500 is 20m, not an out-of-band 14 GHz.
    CHECK(qFuzzyCompare(model.portA().freqKhz, 14161.500));
    CHECK(!model.portA().ptt);

    // The dead port: not live, and reporting a radio name it must not be
    // labelled with.
    CHECK(!model.portB().live);
    CHECK(model.portB().source == QLatin1String("FLEX-8600"));
    CHECK(qFuzzyCompare(model.portB().freqKhz + 1.0, 1.0));

    // ── `tuning`, off the direct wire ─────────────────────────────────
    //
    // Both direct frames carry it, and abortTune() is gated on it — on this
    // transport that guard is the only thing between a STOP press and
    // `autotune`, which on an idle tuner starts one and keys the transmitter.
    // Sourced only from the radio's relayed status, it never moves on a
    // direct-only station: the key never becomes STOP, and pressing it aborts
    // while still reading TUNE.
    CHECK(!model.isTuning());
    {
        QSignalSpy tuning(&model, &TunerModel::tuningChanged);
        peer->write("S0|state bypassA=0 bypassRxA=0 antA=0 bypassB=0 bypassRxB=0 "
                    "antB=0 state=1 tuning=1 bypass=0 relayC1=44 relayL=12 relayC2=8\n");
        peer->flush();
        CHECK(spin([&] { return model.isTuning(); }));
        CHECK(tuning.count() == 1);

        // The status frame carries it too, and ends the tune.
        peer->write("S233|status fwd=21.42 peak=21.42 max=62.43 swr=-60.0000 "
                    "pttA=0 bandA=6 modeA=1 flexA=FLEX-8600 freqA=14161.500 "
                    "bypassA=0 bypassRxA=0 antA=0 "
                    "pttB=0 bandB=0 modeB=0 flexB=FLEX-8600 freqB=0.000 "
                    "bypassB=0 bypassRxB=0 antB=0 "
                    "state=1 active=1 tuning=0 bypass=0 ag=0 "
                    "relayC1=44 relayL=12 relayC2=8\n");
        peer->flush();
        CHECK(spin([&] { return !model.isTuning(); }));
        CHECK(tuning.count() == 2);
    }

    // Keying on the direct path drives the lamps without the radio relaying it.
    QSignalSpy ptt(&model, &TunerModel::pttChanged);
    peer->write("S231|status fwd=36.88 peak=36.88 max=62.43 swr=-60.0000 "
                "pttA=1 bandA=6 modeA=1 flexA=FLEX-8600 freqA=14161.500 "
                "bypassA=0 bypassRxA=0 antA=0 "
                "pttB=0 bandB=0 modeB=0 flexB=FLEX-8600 freqB=0.000 "
                "bypassB=0 bypassRxB=0 antB=0 "
                "state=1 active=1 tuning=0 bypass=0 ag=0 "
                "relayC1=44 relayL=12 relayC2=8\n");
    peer->flush();
    CHECK(spin([&] { return ptt.count() >= 1; }));
    CHECK(model.pttA() && !model.pttB());
    CHECK(model.portA().ptt);

    // An unchanged status does not re-announce: the tuner polls ~20x a second
    // and a repaint per poll is a repaint per poll forever.
    const int settled = ports.count();
    peer->write("S232|status fwd=36.88 peak=36.88 max=62.43 swr=-60.0000 "
                "pttA=1 bandA=6 modeA=1 flexA=FLEX-8600 freqA=14161.500 "
                "bypassA=0 bypassRxA=0 antA=0 "
                "pttB=0 bandB=0 modeB=0 flexB=FLEX-8600 freqB=0.000 "
                "bypassB=0 bypassRxB=0 antB=0 "
                "state=1 active=1 tuning=0 bypass=0 ag=0 "
                "relayC1=44 relayL=12 relayC2=8\n");
    peer->flush();
    spin([&] { return ports.count() > settled; }, 300);
    CHECK(ports.count() == settled);

    // A tune we will not see the end of is dropped on disconnect. Latched, it
    // would pass abortTune()'s guard and command a tuner this client can no
    // longer see.
    peer->write("S0|state bypassA=0 bypassRxA=0 antA=0 bypassB=0 bypassRxB=0 "
                "antB=0 state=1 tuning=1 bypass=0 relayC1=44 relayL=12 relayC2=8\n");
    peer->flush();
    CHECK(spin([&] { return model.isTuning(); }));

    // Losing the tuner drops the readings rather than freezing them: they
    // stop being refreshed, and a stale frequency claims a radio is there.
    peer->close();
    CHECK(spin([&] { return !model.hasPortInfo(); }));
    CHECK(!model.portA().live);
    CHECK(spin([&] { return !model.isTuning(); }));

    conn.disconnect();

    if (g_failures == 0) {
        std::printf("tgxl_direct_protocol_test: all checks passed\n");
        return 0;
    }
    std::printf("tgxl_direct_protocol_test: %d failure(s)\n", g_failures);
    return 1;
}
