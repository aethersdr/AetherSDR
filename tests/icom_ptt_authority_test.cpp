// Socket-free coverage for the Icom PTT seam contract (#5311):
//
//   * setKeying() is COMMAND INTENT. It never publishes transmitChanged on its
//     own; only a decoded CI-V `1C 00` readback moves the published state.
//   * A readback that contradicts a pending UNKEY request is republished even
//     though the backend's own keyed flag did not change — the operator must
//     see a transmitter the radio says is still on the air (Constitution VI).
//   * A readback that contradicts a pending KEY-ON request inside its bounded
//     window is suppressed (RFC #4983's captured FT8 teardown).
//   * The transmit-audio gate follows commanded intent inside that window and
//     radio truth outside it, so voice/DAX/TUNE audio is not head-clipped
//     while the radio completes its PTT transition.
//   * A client unkey clears the derived IC-9700 forward power immediately —
//     zeroing a derived wattage is not an on-air claim.
//
// Frames are injected through the same test seam icom_power_derivation_test
// uses; no session, no UDP peer.
#include "core/backends/icom/IcomCivBackend.h"

#include <QCoreApplication>

#include <cstdint>
#include <iostream>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::icom;

namespace AetherSDR::icom {

struct IcomCivBackendTestAccess {
    static void prepareGeneration(IcomCivBackend& backend, std::uint64_t generation)
    {
        backend.m_connected = true;
        backend.m_sessionGeneration = generation;
    }

    static void selectModel(IcomCivBackend& backend, const IcomModel& model)
    {
        backend.m_model = &model;
        backend.m_frequencyHz = 144'000'000ULL;
    }

    static void deliver(IcomCivBackend& backend, const CivFrame& frame,
                        std::uint64_t generation)
    {
        backend.onCivFrame(frame, generation);
    }

    static bool keyed(const IcomCivBackend& backend) { return backend.m_keyed; }

    static bool pendingIntent(const IcomCivBackend& backend)
    {
        return backend.m_pendingPttIntent.has_value();
    }

    static void expirePendingIntentWindow(IcomCivBackend& backend)
    {
        backend.m_pendingPttUntilMs = 0;
    }

    static bool audioGateOpen(const IcomCivBackend& backend)
    {
        return backend.txAudioGateOpen();
    }
};

} // namespace AetherSDR::icom

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

CivFrame pttReadback(std::uint8_t from, bool keyed)
{
    CivFrame frame;
    frame.to = kControllerAddress;
    frame.from = from;
    frame.cmd = cmd::kControl;
    frame.hasSub = true;
    frame.sub = control::kPtt;
    frame.data = {static_cast<std::uint8_t>(keyed ? 0x01 : 0x00)};
    return frame;
}

void testCommandIsIntentAndReadbackIsState()
{
    const IcomModel* ic705 = modelForCivAddress(0xA4);
    check(ic705 != nullptr, "PTT authority fixture resolves the IC-705");
    if (!ic705) {
        return;
    }

    IcomCivBackend backend;
    IcomCivBackendTestAccess::prepareGeneration(backend, 1);
    IcomCivBackendTestAccess::selectModel(backend, *ic705);

    std::vector<bool> moxPublications;
    QObject::connect(&backend, &IRadioBackend::transmitChanged, &backend,
                     [&](const TransmitDelta& delta) {
        if (delta.mox) {
            moxPublications.push_back(*delta.mox);
        }
    });

    // ---- key-on: intent only -------------------------------------------
    backend.setKeying(true);
    check(moxPublications.empty(),
          "setKeying(true) publishes no transmit edge on its own");
    check(!IcomCivBackendTestAccess::keyed(backend),
          "setKeying(true) does not move the backend's keyed state");
    check(IcomCivBackendTestAccess::pendingIntent(backend),
          "setKeying(true) records a pending PTT intent");
    check(IcomCivBackendTestAccess::audioGateOpen(backend),
          "TX audio is admitted while a key-on intent is pending (no head-clip)");

    // A stale RX answer inside the window must not tear the transmission down.
    IcomCivBackendTestAccess::deliver(backend, pttReadback(0xA4, false), 1);
    check(moxPublications.empty(),
          "a contradictory PTT OFF inside the key-on window is suppressed");
    check(IcomCivBackendTestAccess::pendingIntent(backend),
          "the suppressed answer leaves the key-on intent pending");

    // The radio confirms. THIS is the edge AetherModem waits for.
    IcomCivBackendTestAccess::deliver(backend, pttReadback(0xA4, true), 1);
    check(moxPublications.size() == 1 && moxPublications.back(),
          "the decoded 1C 00 01 readback publishes the keyed edge");
    check(IcomCivBackendTestAccess::keyed(backend),
          "the readback moves the backend's keyed state");
    check(!IcomCivBackendTestAccess::pendingIntent(backend),
          "a confirming readback retires the pending intent");
    check(IcomCivBackendTestAccess::audioGateOpen(backend),
          "TX audio stays admitted once the radio reports keyed");

    // An unchanged keyed answer from the 250 ms poll is confirmation, not a
    // new edge — consumers must not see a 4 Hz stream of "now transmitting".
    // (keyingStateConfirmed itself is scheduler-correlated — it fires only for
    // a readback matched to a read the scheduler issued — so an injected frame
    // cannot observe it here; icom_civ_scheduler_test owns that correlation.)
    IcomCivBackendTestAccess::deliver(backend, pttReadback(0xA4, true), 1);
    check(moxPublications.size() == 1,
          "an unchanged keyed poll answer republishes nothing");

    // ---- unkey: intent only, but the contradiction is never swallowed ----
    backend.setKeying(false);
    check(moxPublications.size() == 1,
          "setKeying(false) publishes no transmit edge on its own");
    check(IcomCivBackendTestAccess::keyed(backend),
          "setKeying(false) leaves the published state keyed until the radio answers");
    check(!IcomCivBackendTestAccess::audioGateOpen(backend),
          "TX audio is gated off as soon as an unkey is commanded");

    // The radio insists it is still transmitting — a lost, refused, or
    // front-panel-overridden unkey. The backend's own flag did not change, so
    // the change-gated path would have gone silent; the contradiction must be
    // republished so RadioModel's optimistic RX presentation is corrected.
    IcomCivBackendTestAccess::deliver(backend, pttReadback(0xA4, true), 1);
    check(moxPublications.size() == 2 && moxPublications.back(),
          "a radio reporting KEYED after an unkey request is republished, "
          "not suppressed by the change gate");
    check(!IcomCivBackendTestAccess::pendingIntent(backend),
          "the contradicting readback retires the unkey intent");
    check(IcomCivBackendTestAccess::audioGateOpen(backend),
          "with no intent pending the audio gate follows radio truth (still keyed)");

    // An obedient radio then unkeys normally.
    IcomCivBackendTestAccess::deliver(backend, pttReadback(0xA4, false), 1);
    check(moxPublications.size() == 3 && !moxPublications.back(),
          "the decoded 1C 00 00 readback publishes the unkeyed edge");
    check(!IcomCivBackendTestAccess::keyed(backend),
          "the readback clears the backend's keyed state");
    check(!IcomCivBackendTestAccess::audioGateOpen(backend),
          "TX audio is gated off once the radio reports RX");

    // ---- the key-on window is BOUNDED -----------------------------------
    backend.setKeying(true);
    check(IcomCivBackendTestAccess::audioGateOpen(backend),
          "a fresh key-on intent admits TX audio again");
    IcomCivBackendTestAccess::expirePendingIntentWindow(backend);
    check(!IcomCivBackendTestAccess::audioGateOpen(backend),
          "an expired key-on window stops admitting audio to an unkeyed radio");
    IcomCivBackendTestAccess::deliver(backend, pttReadback(0xA4, false), 1);
    check(moxPublications.size() == 3,
          "an unconfirmed key-on publishes nothing when the radio still says RX");
    check(!IcomCivBackendTestAccess::pendingIntent(backend),
          "the expired window lets radio truth retire the intent");
}

void testClientUnkeyClearsDerivedForwardPower()
{
    const IcomModel* ic9700 = modelForCivAddress(0xA2);
    check(ic9700 != nullptr, "derived-power fixture resolves the IC-9700");
    if (!ic9700) {
        return;
    }

    IcomCivBackend backend;
    IcomCivBackendTestAccess::prepareGeneration(backend, 1);
    IcomCivBackendTestAccess::selectModel(backend, *ic9700);

    std::vector<bool> moxPublications;
    QObject::connect(&backend, &IRadioBackend::transmitChanged, &backend,
                     [&](const TransmitDelta& delta) {
        if (delta.mox) {
            moxPublications.push_back(*delta.mox);
        }
    });
    bool forwardPowerCleared = false;
    QObject::connect(&backend, &IRadioBackend::meterUpdate, &backend,
                     [&](const QString& name, double value) {
        if (name == QStringLiteral("TX:FWDPWR") && value == 0.0) {
            forwardPowerCleared = true;
        }
    });

    IcomCivBackendTestAccess::deliver(backend, pttReadback(0xA2, true), 1);
    check(moxPublications.size() == 1 && moxPublications.back(),
          "IC-9700 keyed readback publishes the keyed edge");

    forwardPowerCleared = false;
    backend.setKeying(false);
    check(forwardPowerCleared,
          "client-requested Icom unkey immediately clears derived forward power");
    check(moxPublications.size() == 1,
          "clearing the derived wattage does not publish an optimistic RX edge");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testCommandIsIntentAndReadbackIsState();
    testClientUnkeyClearsDerivedForwardPower();
    if (failures == 0) {
        std::cout << "icom_ptt_authority_test: all checks passed\n";
    }
    return failures == 0 ? 0 : 1;
}
