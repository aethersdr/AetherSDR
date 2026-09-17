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

#include <cmath>
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

// A status reply captured with the amplifier keyed, so the meter fields carry
// something to read. Verbatim off 192.168.50.102 on firmware 3.8.9
// (2026-09-15) apart from the sequence number:
//
//   vdd=51.9 id=7.5 peakid=7.5 fwd=42.2 peakfwd=43.6 swr=-60.0 temp=43.9
//
// fwd is dBm (42.2 dBm = 16.6 W) and swr is return loss in dB, reported
// negative on this transport.
QByteArray transmittingStatusReply(const char* seq)
{
    return QByteArray("R") + seq + "|0|state=TRANSMIT_A"
        " bandA=40 bandB=0 bsrcA=FLEX bsrcB=FLEX bsrcAutoA=0 bsrcAutoB=0"
        " flexA=FLEX-8600 flexB=FLEX-8600 vac=246 vdd=51.9 id=7.5 peakid=7.5"
        " fwd=42.2 peakfwd=43.6 swr=-60.0 temp=43.9 cntfreq=0 cat1freq=0"
        " cat2freq=0 hltemp=24.8 biasA=RADIO_AAB biasB=RADIO_AB"
        " fanmode=STANDARD meffa=STANDBY\n";
}

// The status reply, verbatim, with the state word substituted. Everything
// else — the bands, the bias profiles, the FLEX-8600 on both ports — is
// exactly what the amplifier sent.
//
// fwd=30.0 is not a low reading, it is the FLOOR: the amplifier's own FWD
// meter is declared 30.0..63.0 dBm and an idle PGXL rests at exactly 30.0.
QByteArray statusReply(const char* seq, const char* state)
{
    return QByteArray("R") + seq + "|0|state=" + state +
        " bandA=40 bandB=0 bsrcA=FLEX bsrcB=FLEX bsrcAutoA=0 bsrcAutoB=0"
        " flexA=FLEX-8600 flexB=FLEX-8600 vac=245 vdd=0.0 id=0.0 peakid=0.0"
        " fwd=30.0 peakfwd=30.0 swr=-60.0 temp=23.1 cntfreq=0 cat1freq=0"
        " cat2freq=0 hltemp=23.4 biasA=RADIO_AAB biasB=RADIO_AB"
        " fanmode=STANDARD meffa=STANDBY\n";
}

// Everything the client has sent. Accumulated rather than sampled: the check
// is "was this ever sent", and a single readAll() only sees whatever happens
// to be buffered at that instant.
QByteArray g_clientTraffic;

bool peerSaw(QTcpSocket* peer, const char* needle)
{
    peer->waitForReadyRead(20);
    g_clientTraffic += peer->readAll();
    return g_clientTraffic.contains(needle);
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
    peer->write(statusReply("12", "IDLE"));
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
        peer->write(statusReply("13", "IDLE"));
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
        peer->write(statusReply("14", "TRANSMIT_A"));
        peer->flush();
        CHECK(spin([&] { return ports.count() > settled; }));
        CHECK(model.stateText() == QLatin1String("TRANSMIT_A"));
        CHECK(model.portA().ptt);
        CHECK(!model.portB().ptt);
    }
    {
        peer->write(statusReply("15", "TRANSMIT_B"));
        peer->flush();
        CHECK(spin([&] { return model.portB().ptt; }));
        CHECK(!model.portA().ptt);
    }
    {
        peer->write(statusReply("16", "IDLE"));
        peer->flush();
        CHECK(spin([&] { return !model.portB().ptt; }));
        CHECK(!model.portA().ptt);
    }

    // ── Forward power and SWR off the amplifier's own socket ──────────
    //
    // The radio relays AMP/FWD and AMP/RL for the same amplifier, but only
    // when a radio is relaying at all. This is the other source, and it is the
    // only one on a station whose radio publishes no amplifier meters.
    {
        QSignalSpy meters(&model, &AmpModel::directMetersChanged);
        CHECK(meters.isValid());
        peer->write(transmittingStatusReply("17"));
        peer->flush();
        CHECK(spin([&] { return meters.count() >= 1; }));
        const QList<QVariant> last = meters.last();
        // 42.2 dBm = 16.6 W.
        CHECK(std::fabs(last.at(0).toFloat() - 16.6f) < 0.2f);
        // Return loss 60 dB is a matched load: rho = 0.001, SWR 1.002:1.
        CHECK(std::fabs(last.at(1).toFloat() - 1.002f) < 0.01f);

        // Repeated identically on the next poll, and emitted again: a meter
        // that settles on one number is still live, and suppressing the repeat
        // is what freezes a gauge.
        const int settled = meters.count();
        peer->write(transmittingStatusReply("18"));
        peer->flush();
        CHECK(spin([&] { return meters.count() > settled; }));

        // Idle floors the reading at 30.0 dBm — 1 W, not 16.6.
        peer->write(statusReply("19", "IDLE"));
        peer->flush();
        CHECK(spin([&] {
            return !meters.isEmpty()
                && std::fabs(meters.last().at(0).toFloat() - 1.0f) < 0.01f;
        }));
    }

    // ── MEffA, and the shape of a `setup` write ───────────────────────
    //
    // Pinned against a capture of the vendor's own utility (Power Genius
    // Utility 3.8.9 against a PGXL on 3.8.9, 2026-09-15), which was the only
    // way to learn any of it: the User Guide documents the utility's UI and
    // the CAT protocol, never this one.
    {
        // On connect the client asks for the configuration group. It has to:
        // a `setup` WRITE carries every key in the group, so changing one
        // means holding the other four.
        CHECK(peerSaw(peer, "setup read"));

        // MEffA arrives on the STATUS frame, never in the `setup read` reply.
        // Three values exist — ACTIVE, STANDBY and OFF — and the operator
        // controls one bit of them: which of ACTIVE/STANDBY an ENABLED
        // algorithm lands in is the amplifier's call, decided by the PA bias
        // class (class AAB, which SSB and AM select, cannot use it at all).
        peer->write(transmittingStatusReply("20"));
        peer->flush();
        CHECK(spin([&] { return model.meffa() == QLatin1String("STANDBY"); }));
        CHECK(model.meffaEnabled());   // STANDBY is enabled, not disabled

        // It cannot be written yet. The rest of the group is still unknown,
        // and a write that guessed at a nickname or an LED intensity would
        // overwrite real configuration with a guess.
        CHECK(!model.canWriteSetup());
        model.setMeffaEnabled(false);
        spin([&] { return false; }, 150);
        CHECK(!peerSaw(peer, "setup nickname="));

        // Fan mode, however, still goes out — as the single key, which is what
        // shipped before the group write existed. The group form's whole point
        // is carrying the values we are not changing, and here we do not have
        // them; refusing would make fan mode less available than it was, on
        // any firmware that answers `setup read` with an error.
        g_clientTraffic.clear();
        model.setFanMode(QStringLiteral("contest"));
        CHECK(spin([&] { return peerSaw(peer, "setup fanmode=CONTEST"); }));
        CHECK(!g_clientTraffic.contains("nickname="));

        // The `setup read` reply, verbatim off the amplifier. Note that it
        // carries neither meffa nor fanmode — which is exactly why those two
        // are taken off the status frame instead.
        peer->write("R2|0|ledintens=141 txdelay=16 inactivity-timeout=0"
                    " nickname=PowerGeniusXL authcode=\n");
        peer->flush();
        CHECK(spin([&] { return model.canWriteSetup(); }));

        // Now it writes — and it writes the WHOLE group, in the vendor's own
        // order, carrying back the four values it is not changing:
        //   setup nickname=PowerGeniusXL meffa=OFF ledintens=141 fanmode=STANDARD authcode=
        model.setMeffaEnabled(false);
        CHECK(spin([&] {
            return peerSaw(peer, "setup nickname=PowerGeniusXL meffa=OFF"
                                 " ledintens=141 fanmode=STANDARD authcode=");
        }));

        // And no `save`. The vendor's indicator toggle is volatile by design
        // (§9.4) — persisting it is a separate, explicit act, and a panel
        // toggle must not rewrite the amplifier's stored configuration.
        CHECK(!peerSaw(peer, "|save"));

        // Enabling asks for AUTO. The settable vocabulary is NOT the reported
        // one: status says OFF / STANDBY / ACTIVE — what the algorithm is
        // doing — while a write takes AUTO or OFF, whether it may run at all.
        // Sending a reported word back is refused: `meffa=ACTIVE` draws
        // 50000013, and the amplifier does not change. Captured off the vendor
        // utility, which sends AUTO when its checkbox is ticked.
        peer->write("S0|state=IDLE fanmode=CONTEST meffa=OFF\n");
        peer->flush();
        CHECK(spin([&] { return model.meffa() == QLatin1String("OFF"); }));
        CHECK(!model.meffaEnabled());
        model.setMeffaEnabled(true);
        CHECK(spin([&] {
            return peerSaw(peer, "setup nickname=PowerGeniusXL meffa=AUTO"
                                 " ledintens=141 fanmode=CONTEST authcode=");
        }));
        // And never a reported word, which is what the amplifier refuses.
        CHECK(!peerSaw(peer, "meffa=ACTIVE"));
        CHECK(!peerSaw(peer, "meffa=STANDBY"));

        // A fan-mode change takes the same road, carrying MEffA with it. Sent
        // as a single key it would name fanmode and leave the other four out
        // of a write to the group that holds them.
        model.setFanMode(QStringLiteral("broadcast"));
        CHECK(spin([&] {
            return peerSaw(peer, "setup nickname=PowerGeniusXL meffa=AUTO"
                                 " ledintens=141 fanmode=BROADCAST authcode=");
        }));

        // ── And it carries a SETTABLE MEffA word, never a reported one ──
        //
        // This is the whole bug that the AUTO fix above closes, on the other
        // write path. With MEffA enabled the status frame reports STANDBY or
        // ACTIVE; substituting either into the group draws 50000013 and the
        // amplifier keeps its old fan mode — silently, while the panel has
        // already repainted with the new one. An operator on SSB (class AAB,
        // so STANDBY) hit this on every fan change.
        g_clientTraffic.clear();
        peer->write("S0|state=IDLE fanmode=CONTEST meffa=STANDBY\n");
        peer->flush();
        CHECK(spin([&] { return model.meffa() == QLatin1String("STANDBY"); }));
        model.setFanMode(QStringLiteral("standard"));
        CHECK(spin([&] {
            return peerSaw(peer, "setup nickname=PowerGeniusXL meffa=AUTO"
                                 " ledintens=141 fanmode=STANDARD authcode=");
        }));
        CHECK(!g_clientTraffic.contains("meffa=STANDBY"));
        CHECK(!g_clientTraffic.contains("meffa=ACTIVE"));

        // ── The commanded bit outlives the poll that has not caught up ──
        //
        // Between our write and the amplifier's next status the reported word
        // is still the OLD one. Deriving the write from it would send
        // meffa=OFF one poll after the operator enabled MEffA and turn it
        // straight back off.
        g_clientTraffic.clear();
        peer->write("S0|state=IDLE fanmode=CONTEST meffa=OFF\n");
        peer->flush();
        CHECK(spin([&] { return model.meffa() == QLatin1String("OFF"); }));
        model.setMeffaEnabled(true);                 // commands AUTO
        CHECK(spin([&] { return peerSaw(peer, "meffa=AUTO"); }));
        g_clientTraffic.clear();
        model.setFanMode(QStringLiteral("contest")); // device still says OFF
        CHECK(spin([&] {
            return peerSaw(peer, "setup nickname=PowerGeniusXL meffa=AUTO"
                                 " ledintens=141 fanmode=CONTEST authcode=");
        }));
        CHECK(!g_clientTraffic.contains("meffa=OFF"));

        // ── A refusal releases an outstanding `setup read` ──
        //
        // An error reply is `R<seq>|<code>|` with an EMPTY body, so it reads
        // as "nothing to parse" unless the code is looked at. Left unread, a
        // refused `setup read` would keep canWriteSetup() false for the life
        // of the connection and both MEffA and fan mode would be inert.
        QSignalSpy refused(&conn, &PgxlConnection::commandRefused);
        CHECK(refused.isValid());
        peer->write("R99|50000013|\n");
        peer->flush();
        CHECK(spin([&] { return refused.count() >= 1; }));
        CHECK(refused.last().at(1).toString() == QLatin1String("50000013"));
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
