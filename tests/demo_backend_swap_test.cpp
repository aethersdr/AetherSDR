// RadioModel + SimBackend integration: the invariants a demo <-> real backend swap
// has to hold (RFC #4288 / #4473).
//
// These are deliberately at the RadioModel level rather than inside SimBackend.
// sim_backend_test covers SimBackend on its own and passed throughout, yet three
// separate demo-breaking regressions shipped in the review round — every one of
// them lived in how the MODEL wires the backend, which a backend-only test cannot
// see:
//
//   R1  a backend swap must ANNOUNCE itself exactly once, because both rewirings
//       hang off that announcement: MainWindow rebinds the backend-owned seam
//       signals (audioFrameReady -> AudioEngine, the demo's ANF/NB and VFO/mode
//       forwards) and rebinds the PanadapterStream sinks. There were once two
//       signals for this — backendChanged and backendRebuilt — and each revision
//       emitted only one of them, so half the wiring dangled: first the pan sinks,
//       then (after the fix) the whole seam, leaving the demo with no RX audio.
//   R2  connecting the demo must not raise a connection error. The 10 s VITA-49
//       health watchdog asks "did UDP arrive on the pan stream?", which is
//       meaningless for a backend whose spectrum comes over the seam — it fired on
//       every demo connect and put the UI into an error state.
//   R3  the lifecycle must be reported exactly ONCE. SimBackend is a Route A
//       hybrid: it vends a synthetic RadioConnection AND re-emits that
//       connection's lifecycle as its own seam signals, so a model that listens to
//       both hears every connect twice.

#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "core/RadioDiscovery.h"
#include "core/backends/sim/SimBackend.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTimer>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
    else     { std::fprintf(stderr, "[ OK ] %s\n", what); }
}

static void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// The demo entry exactly as ConnectionPanel::addDemoRadio() builds it: the family
// is what selects the backend, the serial is what makes the vended RadioConnection
// run its synthetic connect instead of dialing a socket.
static RadioInfo demoInfo()
{
    RadioInfo i;
    i.name    = QStringLiteral("FLEX-6700");
    i.model   = SimBackend::demoModelName();
    i.serial  = SimBackend::demoSerial();
    i.family  = SimBackend::familyName();
    i.address = QHostAddress(QHostAddress::LocalHost);   // synthetic; never dialed
    i.port    = 4992;
    return i;
}

static RadioInfo flexInfo()
{
    RadioInfo i;
    i.family  = QStringLiteral("flex");
    i.serial  = QStringLiteral("1234-5678-9012-3456");
    i.address = QHostAddress(QStringLiteral("192.0.2.2"));   // TEST-NET-1, unroutable
    i.port    = 4992;
    return i;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    RadioModel model;
    check(model.family() == QLatin1String("flex"), "starts on the Flex backend");
    check(dynamic_cast<SimBackend*>(model.backend()) == nullptr,
          "startup backend is not the simulator");

    QSignalSpy rebuiltSpy(&model, &RadioModel::backendRebuilt);
    QSignalSpy stateSpy(&model, &RadioModel::connectionStateChanged);
    QSignalSpy errSpy(&model, &RadioModel::connectionError);

    // ---- flex -> sim ----
    model.connectToRadio(demoInfo());

    check(model.family() == QLatin1String("sim"), "demo target selects family sim");
    check(dynamic_cast<SimBackend*>(model.backend()) != nullptr,
          "demo target builds a SimBackend (not a FlexBackend)");
    check(rebuiltSpy.count() == 1,
          "R1: the swap announces itself exactly once (seam + pan rewiring hang off this)");

    // The synthetic connect completes asynchronously on the connection thread.
    for (int i = 0; i < 40 && stateSpy.isEmpty(); ++i)
        spin(50);

    int connectedReports = 0;
    for (const auto& args : stateSpy)
        if (args.at(0).toBool()) ++connectedReports;
    check(connectedReports == 1,
          "R3: connecting the demo reports connected EXACTLY once");
    check(model.isConnected(), "model reports connected on the demo");

    // ---- R2: the 10 s VITA-49 health watchdog must not fire for the demo ----
    // Waiting it out is the point of the assertion: the watchdog is armed by the
    // connect itself, so anything short of the full window proves nothing. (It was
    // armed twice before R3 was fixed, and fired both times.)
    spin(10'600);
    if (!errSpy.isEmpty())
        std::fprintf(stderr, "  first error was: %s\n",
                     qPrintable(errSpy.first().at(0).toString()));
    check(errSpy.isEmpty(),
          "R2: no connection error within the UDP-health window on the demo");
    check(model.isConnected(), "still connected after the watchdog window");

    // ---- R4: exactly one pan, and it is the wire-claimed one (#4671) ----
    // SimBackend claims its pan from its own synthetic wire status AND emits the
    // seam's panCenterBandwidthChanged. The Gap B materialise path treated every
    // non-Flex backend as wire-less, so the seam edge minted a SECOND, ownerless
    // pan in the neutral 0xE1000000 space — a ghost "Slice A" the user could
    // neither use nor delete. Both edges have long arrived by this point in the
    // test, so a duplicate would be visible here.
    {
        const auto pans = model.panadapters();
        for (const PanadapterModel* p : pans)
            std::fprintf(stderr, "  pan present: %s\n", qPrintable(p->panId()));
        check(pans.size() == 1,
              "R4: the demo session holds exactly one panadapter (no neutral ghost)");
        bool neutralGhost = false;
        for (const PanadapterModel* p : pans)
            if (p->panId().startsWith(QStringLiteral("0xe1"), Qt::CaseInsensitive))
                neutralGhost = true;
        check(!neutralGhost,
              "R4: no pan lives in the neutral 0xE1000000 space on the demo");
    }

    // ---- sim -> flex round trip ----
    model.connectToRadio(flexInfo());
    check(model.family() == QLatin1String("flex"), "round-trip: back to family flex");
    check(dynamic_cast<SimBackend*>(model.backend()) == nullptr,
          "round-trip: the simulator is gone");
    check(rebuiltSpy.count() == 2, "R1: the swap back announces itself too");
    check(model.slices().isEmpty(),
          "no slice models carried across the family switches");

    if (g_failures == 0)
        std::fprintf(stderr, "demo_backend_swap_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
