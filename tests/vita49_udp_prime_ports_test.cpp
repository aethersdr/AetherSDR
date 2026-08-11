// Regression guard for the VITA-49 UDP prime port set (#4926).
//
// The radio *listens* for the one-byte prime on 4992 but *streams* VITA-49
// back from source port 4993. Priming only 4992 works on a flat LAN and fails
// completely over a routed VPN, where a stateful hop drops the 4993 flow for
// want of conntrack state — the radio connects and the panadapter stays dead.
//
// That failure reproduces only over a real tunnel, so a refactor that dropped
// 4993 would pass every other test and every LAN smoke check. This pins the
// port set at the socket: start() must put a prime datagram on BOTH ports.
//
// Two consequences of binding the literal ports, neither of which bites CI
// (only Linux runs ctest) and both of which bite a developer locally:
//
//  - Two concurrent runs of the suite on one machine collide on 4992/4993 and
//    one of them fails for a reason that is not a regression. The test says so
//    explicitly rather than passing silently, but the message is easy to read
//    as a real failure. Fixed ports are the point — an ephemeral pair would
//    stop pinning the port set — so this is a caveat, not a defect.
//  - start() picks its bind address via chooseLanBindAddress(). Here the
//    explicit and session addresses are unset and the loopback TCP attempt
//    never establishes, so it falls through to AnyIPv4 and the kernel routes
//    the prime out of loopback. If a future change makes start() bind a real
//    interface address instead, sending to 127.0.0.1 from a non-loopback
//    source still works on Linux but not reliably on macOS or the BSDs.

#include "core/PanadapterStream.h"
#include "core/RadioConnection.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QThread>
#include <QUdpSocket>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// Drain a socket, reporting whether a one-byte prime datagram showed up.
static bool sawPrime(QUdpSocket& sock)
{
    bool seen = false;
    while (sock.hasPendingDatagrams()) {
        QByteArray d;
        d.resize(int(sock.pendingDatagramSize()));
        sock.readDatagram(d.data(), d.size());
        if (d.size() == 1 && d.at(0) == '\0')
            seen = true;
    }
    return seen;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // Stand in for the radio's two VITA-49 UDP ports. These are fixed by the
    // protocol, so the test has to hold exactly them — if the environment has
    // them occupied we cannot tell a missing prime from a busy port, and say so
    // rather than reporting a pass.
    QUdpSocket regPort, streamPort;
    if (!regPort.bind(QHostAddress::LocalHost, 4992)
        || !streamPort.bind(QHostAddress::LocalHost, 4993)) {
        std::fprintf(stderr,
                     "FAIL: could not bind 127.0.0.1:4992/4993 in this environment "
                     "(port busy, not a code regression)\n");
        return 1;
    }

    RadioConnection conn;
    conn.init();
    // Sets the radio address synchronously; the TCP attempt against loopback
    // will not complete and does not need to — start() only reads the address.
    conn.connectToHost(QHostAddress::LocalHost, 4992);

    PanadapterStream pan;
    pan.init();
    check(pan.start(&conn), "PanadapterStream::start() returned false");

    bool primedRegister = false;
    bool primedStream   = false;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 3000 && !(primedRegister && primedStream)) {
        app.processEvents();
        primedRegister |= sawPrime(regPort);
        primedStream   |= sawPrime(streamPort);
        QThread::msleep(10);
    }

    check(primedRegister, "no prime datagram on 4992 — the radio never learns our endpoint");
    check(primedStream,
          "no prime datagram on 4993 — VITA-49 return path stays closed on a routed VPN");

    pan.stop();
    conn.disconnectFromRadio();

    if (g_failures == 0)
        std::printf("PASS: UDP prime reaches both 4992 and 4993\n");
    return g_failures == 0 ? 0 : 1;
}
