#include "core/KiwiSdrTxMutePolicy.h"

#include <iostream>

using AetherSDR::KiwiSdrTxMaskWatchdog;
using AetherSDR::KiwiSdrTxMuteLatch;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

// The production predicate itself (KiwiSdrTxMutePolicy.h), so mutating the
// clause breaks this suite — MainWindow::kiwiSdrTransmitMuteRequired() calls
// the same function and only adds its full-duplex/RADE terms on top.
bool muteRequired(const KiwiSdrTxMuteLatch& latch, bool localTx,
                  bool radioTx)
{
    return AetherSDR::kiwiSdrTxMuteRequiredForLatch(latch, localTx, radioTx);
}

bool feed(KiwiSdrTxMuteLatch& latch, bool localTx, bool radioTx)
{
    latch.update(localTx, radioTx);
    return muteRequired(latch, localTx, radioTx);
}

} // namespace

int main()
{
    bool ok = true;

    {
        // Local PTT cycle: mute on key, unmute on the optimistic local
        // unkey even though the interlock still reports TRANSMITTING.
        KiwiSdrTxMuteLatch latch;
        ok &= expect(!feed(latch, false, false), "idle: unmuted");
        ok &= expect(feed(latch, true, false),
                     "local key before interlock ack: muted");
        ok &= expect(feed(latch, true, true), "radio confirms: still muted");
        ok &= expect(!feed(latch, false, true),
                     "local unkey, interlock tail: unmuted optimistically");
        ok &= expect(!feed(latch, false, false),
                     "interlock clears: still unmuted");
    }

    {
        // Externally keyed TX (VOX/CAT/other client): no local edge exists,
        // so the radio term must keep muting until the interlock clears.
        KiwiSdrTxMuteLatch latch;
        ok &= expect(feed(latch, false, true), "external TX: muted");
        ok &= expect(feed(latch, false, true), "external TX holds: muted");
        ok &= expect(!feed(latch, false, false), "external TX ends: unmuted");
    }

    {
        // Re-key during the previous over's interlock tail re-engages the
        // mute; the stale mask must not leak into the new over.
        KiwiSdrTxMuteLatch latch;
        feed(latch, true, true);
        ok &= expect(!feed(latch, false, true), "unkey tail: unmuted");
        ok &= expect(feed(latch, true, true), "re-key during tail: muted");
        ok &= expect(!feed(latch, false, true),
                     "second unkey tail: unmuted again");
        ok &= expect(!feed(latch, false, false), "idle again: unmuted");
    }

    {
        // External TX starting immediately after our unkey tail, without an
        // interlock falling edge in between, is indistinguishable from our
        // own tail and stays unmuted until the caller's mask timeout fires
        // (MainWindow bounds the mask at 2500 ms). Once the interlock does
        // drop and rise again, the mute is honored directly.
        KiwiSdrTxMuteLatch latch;
        feed(latch, true, true);
        feed(latch, false, true);
        ok &= expect(!muteRequired(latch, false, true),
                     "external TX inside our tail window: unmuted (bounded)");
        feed(latch, false, false);
        ok &= expect(feed(latch, false, true),
                     "external TX after interlock cycle: muted");
    }

    {
        // Mask timeout: expire() hands a still-transmitting interlock back
        // to the radio term (foreign TX must mute), and a later local key
        // starts a clean episode.
        KiwiSdrTxMuteLatch latch;
        feed(latch, true, true);
        ok &= expect(!feed(latch, false, true), "unkey tail: unmuted");
        latch.expire();
        ok &= expect(muteRequired(latch, false, true),
                     "mask expired, interlock still up: muted as foreign");
        ok &= expect(feed(latch, false, true),
                     "post-expiry update keeps muting");
        ok &= expect(!feed(latch, false, false), "interlock clears: unmuted");
        ok &= expect(feed(latch, true, false), "fresh local key: muted");
        ok &= expect(!feed(latch, false, false), "fresh local unkey: unmuted");
    }

    {
        // Foreign tx_client_handle inside our unkey tail: the mask is
        // provably wrong and must end immediately (no 2500 ms wait). Our own
        // tail (handle ours) and unreported ownership (txOwnedByUs stays
        // true) must keep the mask and fall back to the timeout.
        KiwiSdrTxMuteLatch latch;
        feed(latch, true, true);
        feed(latch, false, true);
        ok &= expect(latch.radioTermMasked(), "own tail: masked");
        ok &= expect(!AetherSDR::kiwiSdrTxMaskProvablyForeign(
                         latch, true, true),
                     "own tail, our handle: mask kept");
        ok &= expect(!AetherSDR::kiwiSdrTxMaskProvablyForeign(
                         latch, false, false),
                     "interlock down: nothing to expire");
        ok &= expect(AetherSDR::kiwiSdrTxMaskProvablyForeign(
                         latch, true, false),
                     "foreign handle while masked: expire now");
        latch.expire();
        ok &= expect(muteRequired(latch, false, true),
                     "after foreign expiry: radio term mutes again");
        ok &= expect(!AetherSDR::kiwiSdrTxMaskProvablyForeign(
                         latch, true, false),
                     "unmasked latch: foreign check is a no-op");
    }

    {
        // Mask watchdog bookkeeping: arm only when a mask episode begins,
        // and a fired timer may expire only its own episode.
        KiwiSdrTxMaskWatchdog watchdog;
        ok &= expect(watchdog.onMaskChanged(true),
                     "mask rises: caller must arm the timeout");
        const auto armed = watchdog.epoch;
        ok &= expect(watchdog.shouldExpire(armed, true),
                     "same episode, still masked: timer may expire");
        ok &= expect(!watchdog.shouldExpire(armed, false),
                     "mask already down: stale timer no-ops");
        ok &= expect(!watchdog.onMaskChanged(false),
                     "mask falls (re-key during tail): no new timer");
        ok &= expect(!watchdog.shouldExpire(armed, true),
                     "superseded episode: stale timer no-ops even if masked");
        ok &= expect(watchdog.onMaskChanged(true),
                     "next unkey tail: arm again");
        ok &= expect(!watchdog.shouldExpire(armed, true),
                     "old epoch stays invalid across episodes");
        ok &= expect(watchdog.shouldExpire(watchdog.epoch, true),
                     "new episode's own epoch expires normally");
    }

    if (!ok) {
        std::cout << "kiwi_sdr_tx_mute_policy_test FAILED\n";
        return 1;
    }
    std::cout << "kiwi_sdr_tx_mute_policy_test passed\n";
    return 0;
}
