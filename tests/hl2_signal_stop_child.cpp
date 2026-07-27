// Child process for hl2_signal_stop_test.py — not useful on its own.
//
//   ./hl2_signal_stop_child <port>
//
// Starts a real MetisClient against a fake radio on 127.0.0.1:<port>, prints
// "READY", then blocks. The parent kills it and checks that a metis-stop
// datagram arrived.
//
// The point is that this exercises the SHIPPING path: the same
// installEmergencyStopSignalHandlers() main() calls, the same arm() inside
// MetisClient::start(), and a real signal delivered by a real kill(2). A unit
// test that called fireEmergencyStop() directly would prove only that sendto()
// works — it would not prove the handler is installed, that arming happened, or
// that the descriptor is still valid at the moment the process dies.

#include "core/backends/hl2/Hl2EmergencyStop.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QHostAddress>

#include <cstdio>
#include <string>
#include <cstdlib>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: hl2_signal_stop_child <port>\n");
        return 2;
    }
    const quint16 port = static_cast<quint16>(std::atoi(argv[1]));

    // --no-handlers reproduces the ORIGINAL bug on purpose, so the driver can
    // assert that no stop arrives without it. Without this negative control the
    // test would still pass if the handler were never installed and something
    // else happened to emit a stop on the way out — and a test that cannot fail
    // for the right reason is not evidence.
    bool installHandlers = true;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--no-handlers")
            installHandlers = false;
    }
    if (installHandlers)
        AetherSDR::hl2::installEmergencyStopSignalHandlers();

    AetherSDR::hl2::MetisClient client;
    AetherSDR::hl2::MetisClient::Params p;
    p.host = QHostAddress(QStringLiteral("127.0.0.1"));
    p.port = port;
    p.rxFrequencyHz = 14'200'000;
    if (!client.start(p)) {
        std::fprintf(stderr, "child: could not start MetisClient\n");
        return 2;
    }

    std::printf("READY\n");
    std::fflush(stdout);
    return app.exec();   // the parent kills us out of here
}
