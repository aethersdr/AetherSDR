// IcomCIV live CI-V ADDRESS probe — REQUIRES A REAL RADIO, so like
// icom_live_probe it is EXCLUDE_FROM_ALL and not registered with add_test().
//
//   cmake --build build --target icom_live_civ_probe
//   ICOM_USER=... ICOM_PW=... ./build/icom_live_civ_probe <host> [civ-hex] [pinned]
//
// CREDENTIALS COME FROM THE ENVIRONMENT, NOT argv. A password in argv is
// readable by every user on the box through /proc/<pid>/cmdline for as long as
// the probe runs, and it lands in shell history and in any transcript of the
// session that launched it. The radio's RS-BA1 password is additionally worth
// protecting badly: the protocol obfuscates it with a fixed substitution table
// rather than encrypting it, so it is already weak on the wire.
//
// Answers ONE question, on hardware, in the terms the operator experiences it:
//
//     does the session actually carry state, or is it connected and deaf?
//
// A wrong CI-V address fails in complete silence — the radio ignores the frame,
// there is no error, and the session looks healthy — so "it connected" proves
// nothing at all. The verdict below is therefore keyed on a FREQUENCY ARRIVING,
// which is the cheapest piece of state that can only exist if something at the
// other end answered.
//
// Deliberately drives IcomCivBackend rather than IcomSession: the address
// resolution being measured lives in the backend, and a probe that reached past
// it would be measuring the transport instead.
//
// WHY IT TAKES THE ADDRESS AS AN ARGUMENT — this is the whole design. Omitting
// it sends NO `icom.civAddress` parameter, which on a build with auto-detect
// means "auto" and on a build without it means the hardcoded 0xA4. So the same
// source file, built in two worktrees, is a true before/after A/B rather than a
// new binary reporting on itself.
//
// RX-ONLY. There is no PTT command anywhere below and TX is never authorised to
// an unattended run.
//
// ⚠ SPACE RUNS MINUTES APART. Rapid RS-BA1 session churn drove the lab IC-9700
// into a CI-V stall that outlasted an 8-minute backoff and cleared only on a
// power cycle (measured 2026-08-14). Treat radio time as the scarce resource.

#include "core/backends/icom/IcomCivBackend.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

using namespace AetherSDR;
using namespace AetherSDR::icom;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString user = QString::fromLocal8Bit(qgetenv("ICOM_USER"));
    const QString pass = QString::fromLocal8Bit(qgetenv("ICOM_PW"));
    if (argc < 2 || user.isEmpty() || pass.isEmpty()) {
        std::fprintf(stderr,
                     "usage: ICOM_USER=... ICOM_PW=... %s <host> [civ-hex] [pinned]\n"
                     "  no civ-hex  -> send no icom.civAddress at all (auto on a\n"
                     "                 build that has auto-detect; 0xA4 on one that\n"
                     "                 does not - that IS the A/B)\n"
                     "  civ-hex     -> a model pick: the wire may correct it\n"
                     "  + pinned    -> a typed address: the wire must NOT correct it\n",
                     argv[0]);
        return 2;
    }
    const QString host = QString::fromLocal8Bit(argv[1]);
    const bool haveCiv = argc > 2;
    const bool pinned  = argc > 3;
    unsigned civ = 0;
    if (haveCiv)
        civ = std::strtoul(argv[2], nullptr, 16);

    IcomCivBackend backend;

    double frequencyMHz = 0.0;
    QString mode;
    QStringList warnings;
    QString connectError;
    bool connected = false;

    QObject::connect(&backend, &IRadioBackend::connected, &app,
                     [&] { connected = true; });
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &app,
                     [&](int, const SliceDelta& d) {
                         if (d.frequency) frequencyMHz = *d.frequency;
                         if (d.mode) mode = *d.mode;
                     });
    QObject::connect(&backend, &IRadioBackend::configurationWarning, &app,
                     [&](const QString& m) { warnings << m; });
    QObject::connect(&backend, &IRadioBackend::connectionError, &app,
                     [&](const QString& m) { connectError = m; });

    RadioConnectRequest req;
    req.host = host;
    req.params.insert(QStringLiteral("icom.username"), user);
    req.params.insert(QStringLiteral("icom.password"), pass);
    if (haveCiv) {
        req.params.insert(QStringLiteral("icom.civAddress"), civ);
        // Harmless on a build that predates it — an unknown params key is simply
        // never read — which is what keeps this one file valid on both sides.
        if (pinned)
            req.params.insert(QStringLiteral("icom.civAddressPinned"), true);
    }

    std::printf("probe: host=%s civ=%s%s\n", qPrintable(host),
                haveCiv ? qPrintable(QString::number(civ, 16).toUpper()) : "(none/auto)",
                pinned ? " PINNED" : "");

    backend.connectRadio(req);

    // Long enough for the RS-BA1 handshake, the CI-V stream, the bounded
    // detect wait and the read burst, with room to spare on a slow link.
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 15000 && frequencyMHz == 0.0)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    // A short tail so a late model resolution and its warnings are captured
    // even once the frequency has landed.
    const qint64 tailUntil = clock.elapsed() + 2000;
    while (clock.elapsed() < tailUntil)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    const IcomModel& m = backend.model();
    std::printf("  session connected : %s\n", connected ? "yes" : "NO");
    std::printf("  device name       : %s\n", qPrintable(backend.healthSnapshot()
                    .values.value(QStringLiteral("model")).toString()));
    std::printf("  model resolved    : %s (CI-V %02X, known=%s)\n",
                std::string(m.name).c_str(), m.civAddress, m.isKnown() ? "yes" : "no");
    std::printf("  frequency         : %.6f MHz\n", frequencyMHz);
    std::printf("  mode              : %s\n", qPrintable(mode));
    for (const QString& w : warnings)
        std::printf("  warning           : %s\n", qPrintable(w));
    if (!connectError.isEmpty())
        std::printf("  connect error     : %s\n", qPrintable(connectError));

    // THE VERDICT, and it is deliberately not "did it connect".
    const bool alive = frequencyMHz > 0.0;
    std::printf("  VERDICT           : %s\n",
                alive ? "ALIVE - the radio answered CI-V"
                      : "DEAD - connected, and nothing answered");

    backend.disconnectRadio();
    // Let the session's teardown datagrams actually leave, so the radio frees
    // the slot promptly. A radio still holding a stale session is how the next
    // run reads as BUSY.
    QElapsedTimer drain;
    drain.start();
    while (drain.elapsed() < 500)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return alive ? 0 : 1;
}
