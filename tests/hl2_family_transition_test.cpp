// Family-transition safety for the first non-Flex backend (#4448). Exercises the
// invariants a Flex <-> HL2 switch must hold, using RadioModel's real backend
// swap: connectToRadio() rebuilds the backend synchronously before any network
// I/O, so an unroutable address reaches the post-swap state without hardware.
//
//   F2  RX-only backends refuse keying (capability + enforcement).
//   F3  reboot is a per-family capability (Flex yes, HL2 no).
//   round-trip  Flex -> HL2 -> Flex leaves the model in a clean, Flex-capable
//               state (no crash, capabilities track the live backend).

#include "models/RadioModel.h"
#include "models/TransmitModel.h"
#include "core/RadioDiscovery.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

static RadioInfo hl2Info()
{
    RadioInfo i;
    i.family  = QStringLiteral("hl2");
    i.serial  = QStringLiteral("00:1C:C0:00:00:01");
    i.address = QHostAddress(QStringLiteral("192.0.2.1"));   // TEST-NET-1, unroutable
    i.port    = 1024;
    return i;
}

static RadioInfo flexInfo()
{
    RadioInfo i;
    i.family  = QStringLiteral("flex");
    i.serial  = QStringLiteral("1234-5678-9012-3456");
    i.address = QHostAddress(QStringLiteral("192.0.2.2"));
    i.port    = 4992;
    return i;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- Default is Flex: transmits, reboots ----
    RadioModel model;
    check(model.backendCapabilities().canTransmit,
          "Flex default advertises canTransmit");
    check(model.backendCapabilities().canReboot,
          "Flex default advertises canReboot");

    // ---- Switch to HL2 (RX-only) ----
    model.connectToRadio(hl2Info());
    check(!model.backendCapabilities().canTransmit,
          "F2: HL2 advertises canTransmit=false");
    check(!model.backendCapabilities().canReboot,
          "F3: HL2 advertises canReboot=false");
    check(model.panStream() == nullptr,
          "HL2 owns no PanadapterStream");

    // F2: keying an RX-only backend must be refused before any TX state is set.
    check(!model.transmitModel().isTransmitting(), "not transmitting before key");
    model.setTransmit(true, TransmitModel::PttSource::Mox);
    check(!model.transmitModel().isTransmitting(),
          "F2: setTransmit(true) on an RX-only backend does not enter TX");
    model.setTransmit(true, TransmitModel::PttSource::Tune);
    check(!model.transmitModel().isTransmitting(),
          "F2: TUNE carrier on an RX-only backend does not enter TX");

    // F3: reboot on an RX-only backend is a no-op, not a crash. (Not connected,
    // so it returns early regardless; the point is it must not dereference the
    // absent RadioConnection.)
    model.rebootRadio();
    check(true, "F3: rebootRadio() on HL2 did not crash");

    // ---- Round-trip back to Flex ----
    model.connectToRadio(flexInfo());
    check(model.backendCapabilities().canTransmit,
          "round-trip: Flex regains canTransmit after HL2 -> Flex");
    check(model.backendCapabilities().canReboot,
          "round-trip: Flex regains canReboot after HL2 -> Flex");
    check(model.panStream() != nullptr,
          "round-trip: Flex owns a PanadapterStream again");

    // Flex -> HL2 -> same Flex: nothing from the HL2 session lingers as a
    // reclaim candidate (F1). No slices should have survived the switches.
    check(model.slices().isEmpty(),
          "F1: no slice models carried across the family switches");

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_family_transition_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
