#include "core/KiwiSdrTxMutePolicy.h"
// The resume hold has to add the presentation holdback the audio engine
// really applies, which is routed here — so the two are pinned together.
#include "core/ReceivePresentationSync.h"

#include <QString>

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

    {
        // Resume-hold computation ("Resume audio after TX delay").
        using AetherSDR::kiwiSdrResumeHoldMs;
        using AetherSDR::kKiwiSdrResumeHoldGuardMs;
        using AetherSDR::kKiwiSdrResumeHoldMinMs;
        using AetherSDR::kKiwiSdrResumeHoldMaxMs;

        ok &= expect(kiwiSdrResumeHoldMs(true, 500, 520, 0)
                         == 500 + kKiwiSdrResumeHoldGuardMs,
                     "valid estimate: offset + guard");
        ok &= expect(kiwiSdrResumeHoldMs(true, 500, 520, 520)
                         == 500 + 520 + kKiwiSdrResumeHoldGuardMs,
                     "applied presentation holdback adds to the hold");
        ok &= expect(kiwiSdrResumeHoldMs(false, 0, 520, 520)
                         == 520 + 520 + kKiwiSdrResumeHoldGuardMs,
                     "no estimate: base latency proxy + holdback + guard");
        ok &= expect(kiwiSdrResumeHoldMs(true, -300, 520, 0)
                         == kKiwiSdrResumeHoldMinMs,
                     "negative estimate clamps to minimum hold");
        ok &= expect(kiwiSdrResumeHoldMs(true, -300, 520, -100)
                         == kKiwiSdrResumeHoldMinMs,
                     "negative holdback is ignored, clamps to minimum");
        ok &= expect(kiwiSdrResumeHoldMs(true, 10000, 520, 0)
                         == kKiwiSdrResumeHoldMaxMs,
                     "huge estimate clamps to maximum hold");
        ok &= expect(kiwiSdrResumeHoldMs(true, 3000, 520, 5000)
                         == kKiwiSdrResumeHoldMaxMs,
                     "estimate + holdback together clamp to maximum");
        ok &= expect(kiwiSdrResumeHoldMs(false, 0, 0, 0)
                         == kKiwiSdrResumeHoldMinMs,
                     "zero fallback clamps to minimum hold");
    }

    {
        // Which ingest-lag figure the hold is entitled to use. This is the
        // decision half of "Resume audio after TX delay" — a wrong answer
        // here does not fail anything at runtime, it just silently mutes the
        // operator for the wrong length of time, so every branch is pinned.
        using AetherSDR::KiwiSdrResumeHoldSyncInputs;
        using AetherSDR::kiwiSdrResumeHoldIngestLag;

        // A locked AutoAssist estimate, measured against this very profile.
        KiwiSdrResumeHoldSyncInputs locked;
        locked.isSyncTarget = true;
        locked.syncEnabled = true;
        locked.autoAssist = true;
        locked.estimateValid = true;
        locked.estimateConfidence = 0.8f;
        locked.autoLockConfidence = 0.35f;
        locked.estimateOffsetMs = 900;

        auto lag = kiwiSdrResumeHoldIngestLag(locked);
        ok &= expect(lag.valid && lag.offsetMs == 900,
                     "AutoAssist, confident estimate on the target: used");

        // The sync engine acts on a HELD estimate even below the lock
        // threshold; the hold must make the same call, or it would flip
        // between measured and proxy on every over while sync itself holds.
        auto held = locked;
        held.estimateConfidence = 0.05f;
        held.estimateHeld = true;
        lag = kiwiSdrResumeHoldIngestLag(held);
        ok &= expect(lag.valid && lag.offsetMs == 900,
                     "AutoAssist, held estimate below threshold: still used");

        auto lowConfidence = locked;
        lowConfidence.estimateConfidence = 0.34f;
        ok &= expect(!kiwiSdrResumeHoldIngestLag(lowConfidence).valid,
                     "AutoAssist, unheld estimate under lock: falls back");

        auto invalid = locked;
        invalid.estimateValid = false;
        ok &= expect(!kiwiSdrResumeHoldIngestLag(invalid).valid,
                     "AutoAssist, invalid estimate: falls back");

        // The measurement describes ONE profile's chain. A second Kiwi is a
        // different network path entirely — this is the guard whose absence
        // would hand every profile the target's ingest lag.
        auto otherProfile = locked;
        otherProfile.isSyncTarget = false;
        ok &= expect(!kiwiSdrResumeHoldIngestLag(otherProfile).valid,
                     "not the sync target: estimate does not apply");

        auto syncOff = locked;
        syncOff.syncEnabled = false;
        ok &= expect(!kiwiSdrResumeHoldIngestLag(syncOff).valid,
                     "receive sync disabled: estimate does not apply");

        // Manual mode: the operator's own offset, but only when it says the
        // Kiwi arrives LATE. Zero or negative describes the other direction
        // and carries no ingest-lag information.
        KiwiSdrResumeHoldSyncInputs manual;
        manual.isSyncTarget = true;
        manual.syncEnabled = true;
        manual.autoAssist = false;
        manual.manualOffsetMs = 1200;
        lag = kiwiSdrResumeHoldIngestLag(manual);
        ok &= expect(lag.valid && lag.offsetMs == 1200,
                     "Manual, positive offset on the target: used");

        auto manualZero = manual;
        manualZero.manualOffsetMs = 0;
        ok &= expect(!kiwiSdrResumeHoldIngestLag(manualZero).valid,
                     "Manual, zero offset: falls back");

        auto manualNegative = manual;
        manualNegative.manualOffsetMs = -400;
        ok &= expect(!kiwiSdrResumeHoldIngestLag(manualNegative).valid,
                     "Manual, negative offset: falls back");

        // Mode discipline: a stale AutoAssist estimate must not leak into
        // Manual mode, and a manual offset must not leak into AutoAssist.
        auto manualWithStaleEstimate = manual;
        manualWithStaleEstimate.estimateValid = true;
        manualWithStaleEstimate.estimateConfidence = 0.9f;
        manualWithStaleEstimate.autoLockConfidence = 0.35f;
        manualWithStaleEstimate.estimateOffsetMs = 900;
        lag = kiwiSdrResumeHoldIngestLag(manualWithStaleEstimate);
        ok &= expect(lag.valid && lag.offsetMs == 1200,
                     "Manual mode ignores the AutoAssist estimate");

        auto autoWithManualOffset = locked;
        autoWithManualOffset.estimateValid = false;
        autoWithManualOffset.manualOffsetMs = 1200;
        ok &= expect(!kiwiSdrResumeHoldIngestLag(autoWithManualOffset).valid,
                     "AutoAssist mode ignores the manual offset");
    }

    {
        // The holdback fed to the hold must be the one the MIX applies —
        // i.e. whatever AudioEngine::setReceivePresentationDelays() derives
        // through receivePresentationExternalKiwiDelayMs() (that routing
        // itself is pinned in receive_presentation_sync_test). Under
        // AutoAssist a non-target source is played with NO holdback, so
        // sourcing the figure anywhere else runs the hold long and swallows
        // real post-TX audio instead of the operator's own tail.
        using AetherSDR::kiwiSdrResumeHoldMs;
        using AetherSDR::kKiwiSdrResumeHoldGuardMs;
        using AetherSDR::receivePresentationExternalKiwiDelayMs;

        const QString target = QStringLiteral("profile-A");
        const QString other = QStringLiteral("profile-B");
        constexpr int kEngineKiwiDelayMs = 900;
        constexpr int kFallbackBaseMs = 360;

        ok &= expect(kiwiSdrResumeHoldMs(
                         false, 0, kFallbackBaseMs,
                         receivePresentationExternalKiwiDelayMs(
                             target, target, kEngineKiwiDelayMs))
                         == kFallbackBaseMs + kEngineKiwiDelayMs
                             + kKiwiSdrResumeHoldGuardMs,
                     "sync target: hold covers the holdback it is played at");
        ok &= expect(kiwiSdrResumeHoldMs(
                         false, 0, kFallbackBaseMs,
                         receivePresentationExternalKiwiDelayMs(
                             other, target, kEngineKiwiDelayMs))
                         == kFallbackBaseMs + kKiwiSdrResumeHoldGuardMs,
                     "non-target: hold adds no phantom holdback");
    }

    if (!ok) {
        std::cout << "kiwi_sdr_tx_mute_policy_test FAILED\n";
        return 1;
    }
    std::cout << "kiwi_sdr_tx_mute_policy_test passed\n";
    return 0;
}
