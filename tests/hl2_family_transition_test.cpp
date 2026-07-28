// Family-transition safety for the first non-Flex backend (#4448). Exercises the
// invariants a Flex <-> HL2 switch must hold, using RadioModel's real backend
// swap: connectToRadio() rebuilds the backend synchronously before any network
// I/O, so an unroutable address reaches the post-swap state without hardware.
//
//   TX  HL2 is TX-capable in a GUI session (transmit landed after #4448); the
//       enforcement gate itself (m_txAllowed) is covered by hl2_tx_gate_test.
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
    check(!model.backendCapabilities().hostModulates,
          "#4449: Flex modulates on-radio, not on the host");

    // ---- Switch to HL2 (TX-capable in a GUI session) ----
    // HL2 transmit landed after #4448: a normal (non-automation) session
    // advertises canTransmit=true, matching Flex. Headless automation stays
    // gated behind AETHER_AUTOMATION_ALLOW_TX inside the backend (m_txAllowed).
    model.connectToRadio(hl2Info());
    check(model.backendCapabilities().canTransmit,
          "HL2 advertises canTransmit (transmit landed post-#4448)");
    check(!model.backendCapabilities().canReboot,
          "F3: HL2 advertises canReboot=false");
    check(model.backendCapabilities().hostModulates,
          "#4449: HL2 host-modulates (PC runs the modulator, no on-radio jacks)");
    check(model.panStream() == nullptr,
          "HL2 owns no PanadapterStream");

    // Keying a backend with no live link must not spuriously enter TX,
    // regardless of capability — connectToRadio() reached the post-swap state
    // against an unroutable address, so there is nothing to key.
    check(!model.transmitModel().isTransmitting(), "not transmitting before key");
    model.setTransmit(true, TransmitModel::PttSource::Mox);
    check(!model.transmitModel().isTransmitting(),
          "setTransmit(true) with no live HL2 link does not enter TX");
    model.setTransmit(true, TransmitModel::PttSource::Tune);
    check(!model.transmitModel().isTransmitting(),
          "TUNE carrier with no live HL2 link does not enter TX");

    // F3: reboot on an RX-only backend is a no-op, not a crash. (Not connected,
    // so it returns early regardless; the point is it must not dereference the
    // absent RadioConnection.)
    model.rebootRadio();
    check(true, "F3: rebootRadio() on HL2 did not crash");

    // WSPR beacon (#4435): the beacon rides a Flex `dax_tx` stream, which no
    // other family provides — HL2 host-modulates and has no DAX. It borrows
    // station state (`transmit dax`, the TX filter, DAX TX stream ownership)
    // before it ever keys, so a refusal must leave none of that latched.
    // `transmit dax` in particular is the one that bites: its own comment notes
    // a stuck dax=1 "would silently kill the next mic voice TX on every platform
    // where updateDaxTxMode() is compiled out".
    //
    // Note this is NOT the capabilities().canTransmit gate doing the work — HL2
    // advertises canTransmit since transmit landed, so the gate passes here and
    // the refusal comes from the absent DAX TX stream. The gate remains the
    // fail-closed guard for any family that reports canTransmit=false.
    check(!model.prepareWsprTransmit(),
          "WSPR: prepareWsprTransmit() is refused on a family with no DAX TX");
    check(!model.hasWsprTxStream(),
          "WSPR: a refused prepare claims no TX audio stream");
    check(!model.transmitModel().daxOn(),
          "WSPR: a refused prepare does not leave `transmit dax` latched");

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
