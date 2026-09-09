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
    check(model.backendCapabilities().family == QLatin1String("flex"),
          "isFlexRadio identifies the default Flex backend");
    check(model.backendCapabilities().canTransmit,
          "Flex default advertises canTransmit");
    check(model.backendCapabilities().canReboot,
          "Flex default advertises canReboot");

    RadioInfo networkSeed = flexInfo();
    networkSeed.address = QHostAddress(QStringLiteral("192.0.2.44"));
    model.connectToRadio(networkSeed);
    check(model.ip() == QStringLiteral("192.0.2.44"),
          "connect seeds only the selected radio endpoint");
    model.disconnectFromRadio();
    check(QMetaObject::invokeMethod(&model, "onDisconnected", Qt::DirectConnection),
          "network reset fixture reached RadioModel");
    check(model.ip().isEmpty() && model.netmask().isEmpty()
              && model.gateway().isEmpty() && model.mac().isEmpty(),
          "disconnect clears session-owned network identity");
    check(!model.backendCapabilities().hostModulates,
          "#4449: Flex modulates on-radio, not on the host");

    // ---- Switch to HL2 (TX-capable in a GUI session) ----
    // HL2 transmit landed after #4448: a normal (non-automation) session
    // advertises canTransmit=true, matching Flex. Headless automation stays
    // gated behind AETHER_AUTOMATION_ALLOW_TX inside the backend (m_txAllowed).
    model.connectToRadio(hl2Info());
    check(model.backendCapabilities().family != QLatin1String("flex"),
          "isFlexRadio rejects the HL2 family without a model-name table");
    check(model.backendCapabilities().canTransmit,
          "HL2 advertises canTransmit (transmit landed post-#4448)");
    check(!model.backendCapabilities().canReboot,
          "F3: HL2 advertises canReboot=false");
    check(model.backendCapabilities().hostModulates,
          "#4449: HL2 host-modulates (PC runs the modulator, no on-radio jacks)");
    check(model.panStream() == nullptr,
          "HL2 owns no PanadapterStream");

    // ── The normalized RX-audio bus ────────────────────────────────────────
    //
    // The CW decoder, the RTTY decoder and the QSO recorder's RX tap all ride
    // rxDemodAudioReady. Two properties have to hold, and the second is the one
    // that has actually broken before.
    //
    //   1. It is FED on a radio with no PanadapterStream. That is the whole
    //      point: bound to the stream, these three were silently dead on an HL2.
    //   2. EXACTLY ONE producer is connected. Connecting both is the double-feed
    //      shape of #4490, where the sim's frames arrived over two routes and
    //      the engine consumed at double rate. Here it would hand every decoder
    //      each block twice — which a Morse decoder reads as doubled timing,
    //      i.e. wrong text rather than no text.
    //
    // Re-entering the family swap is what makes (2) worth asserting: the seam
    // relay has `this` on BOTH ends, so unlike a stream-bound connection Qt has
    // nothing to drop for us when the backend is replaced.
    {
        int busBlocks = 0;
        QObject::connect(&model, &RadioModel::rxDemodAudioReady,
                         &model, [&busBlocks](const QByteArray&) { ++busBlocks; });

        const QByteArray frame(256, '\0');
        emit model.backendAudioFrameReady(frame);
        check(busBlocks == 1,
              "RX bus: a seam backend's audio reaches rxDemodAudioReady exactly once");

        // On the Flex leg the seam relay must be GONE, not merely outnumbered.
        // This is the state wireRxDemodAudioBus()'s disconnect exists to
        // produce, and asserting it directly names the failure better than
        // catching a doubled count later would. (PR #4537 review.)
        model.connectToRadio(flexInfo());
        busBlocks = 0;
        emit model.backendAudioFrameReady(frame);
        check(busBlocks == 0,
              "RX bus: the seam relay is dropped when a Flex takes over");

        // Back to HL2: re-enters wireRxDemodAudioBus() with the same endpoints
        // on the seam relay, which is the case Qt cannot clean up for us.
        model.connectToRadio(hl2Info());
        busBlocks = 0;
        emit model.backendAudioFrameReady(frame);
        check(busBlocks == 1,
              "RX bus: still exactly one producer after a family round-trip");
    }

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

    // WSPR beacon (#4435): the beacon used to be refused outright on HL2,
    // because it rides a Flex `dax_tx` stream that no other family provides.
    // It now takes the host-modulated arm instead: the modulator is ours, the
    // AudioEngine pump already reaches it through the m_hostModulation branch
    // of feedDaxTxAudioInternal(), and there is no transport to create.
    check(model.prepareWsprTransmit(),
          "WSPR: prepareWsprTransmit() succeeds on a host-modulating backend");
    check(model.hasWsprTxStream(),
          "WSPR: the host-modulated arm reports a ready TX audio route");

    // The point of the original refusal survives the change: the beacon must
    // still not borrow Flex station state on a radio that has none. `transmit
    // dax` is the one that bites — its own comment notes a stuck dax=1 "would
    // silently kill the next mic voice TX on every platform where
    // updateDaxTxMode() is compiled out". A host-modulating radio has no
    // radio-side modulator to point at DAX in the first place.
    check(!model.transmitModel().daxOn(),
          "WSPR: the host-modulated arm does not latch `transmit dax`");

    model.releaseWsprTransmit();
    check(!model.hasWsprTxStream(),
          "WSPR: release drops the host-modulated route claim");
    check(!model.transmitModel().daxOn(),
          "WSPR: release leaves `transmit dax` clear");

    // Prepare/release is idempotent and re-armable — the dialog defers a frame
    // to the next two-minute slot without re-preparing, but an operator who
    // cancels and re-arms runs this pair again.
    check(model.prepareWsprTransmit() && model.hasWsprTxStream(),
          "WSPR: the host-modulated arm can be re-armed after a release");
    model.releaseWsprTransmit();
    check(!model.hasWsprTxStream(), "WSPR: second release also clears");

    // An ARMED beacon must not carry its route claim onto another radio.
    // connectToRadio() on a family switch runs dropAllSessionModelsForFamily-
    // Switch() -> teardownBackend() -> setupBackend() and reaches neither
    // releaseWsprTransmit() nor onDisconnected(), so the host-modulated latch
    // used to survive — and hasWsprTxStream() would then tell the beacon dialog
    // its route was ready on a FLEX, which keys the radio for a full 111.6 s
    // frame with no dax_tx stream behind it. Unintended transmission, so it is
    // asserted rather than reasoned about. (PR #4537 review, finding 2.)
    check(model.prepareWsprTransmit(), "WSPR: armed on HL2 for the switch test");
    check(model.hasWsprTxStream(), "WSPR: armed claim is live before the switch");
    model.connectToRadio(flexInfo());
    check(model.backendCapabilities().family == QLatin1String("flex"),
          "round-trip: isFlexRadio restores on the Flex family");
    check(!model.hasWsprTxStream(),
          "WSPR: an armed host-modulated claim does NOT survive onto a Flex");
    model.connectToRadio(hl2Info());

    // ---- Round-trip back to Flex ----
    model.connectToRadio(flexInfo());
    check(model.backendCapabilities().canTransmit,
          "round-trip: Flex regains canTransmit after HL2 -> Flex");
    check(model.backendCapabilities().canReboot,
          "round-trip: Flex regains canReboot after HL2 -> Flex");
    check(model.panStream() != nullptr,
          "round-trip: Flex owns a PanadapterStream again");
    // A host-modulated claim must not survive into a family that DOES need a
    // real dax_tx stream, or the dialog would arm against a stream that was
    // never created and transmit a silent 111.6 s frame.
    check(!model.hasWsprTxStream(),
          "round-trip: no WSPR route is claimed on Flex before a prepare");

    // Flex -> HL2 -> same Flex: nothing from the HL2 session lingers as a
    // reclaim candidate (F1). No slices should have survived the switches.
    check(model.slices().isEmpty(),
          "F1: no slice models carried across the family switches");

    // ── Mic gain survives the backend rebuild ───────────────────────────────
    //
    // THE FAILURE THIS PINS. A family switch destroys the backend and builds a
    // fresh one, so the new Hl2TxDsp starts at its own 1.0 default. But the
    // seam carrying mic gain to a host-modulating backend fires on operator
    // INTENT — the slider moving — and a rebuild is not the slider moving.
    // TransmitModel is never reset and micLevel is not persisted, so without an
    // explicit re-assert the slider goes on reading the operator's value while
    // the modulator sits at unity, and the radio transmits several dB below
    // what every readout claims.
    //
    // That is exactly the readback-agrees-with-the-failure shape the mic-gain
    // fix exists to eliminate, displaced one seam over, so it gets its own pin.
    // Break it by deleting the re-assert at the end of
    // RadioModel::setupBackend() and this check fails with micLevel still at
    // its 50 default.
    //
    // Asserted on micLevel rather than on micGainAppliedLinear because the
    // latter is echoed back from the DSP worker thread and would need a running
    // event loop to arrive; the modulator-side half is covered by
    // hl2_txdsp_test and the loopback test. What is proved here is that the
    // fresh backend was told at all.
    model.transmitModel().setMicLevel(80);
    model.connectToRadio(hl2Info());
    {
        const auto snap = model.backendHealthSnapshot();
        check(snap.values.value(QStringLiteral("micLevel")).toInt() == 80,
              "mic gain is re-asserted into a rebuilt backend (slider and "
              "modulator cannot silently part on a family switch)");
    }

    // And the operator's own default is carried just as faithfully: 50 maps to
    // the modulator's 1.0, so the re-assert itself must not nudge a session
    // that never touched the slider off unity.
    model.connectToRadio(flexInfo());
    model.transmitModel().setMicLevel(50);
    model.connectToRadio(hl2Info());
    {
        const auto snap = model.backendHealthSnapshot();
        check(snap.values.value(QStringLiteral("micLevel")).toInt() == 50,
              "an untouched slider still lands on the modulator's unity point");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_family_transition_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
