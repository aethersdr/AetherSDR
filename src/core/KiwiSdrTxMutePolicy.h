#pragma once

#include <algorithm>
#include <cstdint>

// KiwiSDR transmit-mute latch (fork feature: warm Kiwi audio through TX).
//
// The Kiwi transmit gate should release on this client's optimistic local
// unkey (TransmitModel::setMox(false) fires immediately) instead of waiting
// out the radio's interlock round trip and hang timers — but transmissions
// this client never keyed (VOX, CAT, hardware PTT, other clients) must still
// gate on the radio-reported interlock state, which is the only signal that
// exists for them.
//
// The latch distinguishes the two: while a radio-reported TRANSMITTING state
// is only the tail of a transmission this client already ended locally,
// radioTermMasked() is true and the caller ignores the radio term in its
// mute predicate. The latch clears on the interlock's falling edge.

namespace AetherSDR {

struct KiwiSdrTxMuteLatch {
    bool localTxSeen{false};
    bool localUnkeyPending{false};

    // Feed from every TX-state signal edge, before evaluating the mute
    // predicate. localTxActive is this client's optimistic view
    // (isTransmitting() || isTuning()); radioTransmitting is the raw
    // interlock state.
    void update(bool localTxActive, bool radioTransmitting)
    {
        if (localTxActive) {
            localTxSeen = true;
            localUnkeyPending = false;
        } else if (!radioTransmitting) {
            localTxSeen = false;
            localUnkeyPending = false;
        } else if (localTxSeen) {
            localUnkeyPending = true;
        }
    }

    bool radioTermMasked() const { return localUnkeyPending; }

    // The mask rides this client's own unkey tail, but nothing in the
    // interlock stream distinguishes that tail from a foreign transmission
    // that begins before the interlock falls — so the caller must bound the
    // mask with a timeout. expire() ends the mask AND hands the rest of the
    // episode to the radio term (a still-TRANSMITTING interlock this long
    // after our unkey is someone else's transmission, which must mute).
    void expire()
    {
        localTxSeen = false;
        localUnkeyPending = false;
    }
};

// The latch half of MainWindow::kiwiSdrTransmitMuteRequired(), shared so the
// unit test exercises the production clause instead of a copy: mute on the
// local optimistic TX view, or on a radio-reported TX whose interlock term is
// not masked as our own unkey tail. Caller-side terms that need MainWindow
// state (full-duplex bypass, RADE EOO) stay with the caller.
inline bool kiwiSdrTxMuteRequiredForLatch(const KiwiSdrTxMuteLatch& latch,
                                          bool localTxActive,
                                          bool radioTransmitting)
{
    return localTxActive || (radioTransmitting && !latch.radioTermMasked());
}

// A masked radio term is only ever this client's own unkey tail. When the
// interlock names a transmitter that is not this client, the mask is provably
// wrong and must end now instead of riding out the caller's timeout bound.
// txOwnedByUs must stay true when ownership is unknown (no tx_client_handle
// reported), so the unprovable case still falls back to the timeout.
inline bool kiwiSdrTxMaskProvablyForeign(const KiwiSdrTxMuteLatch& latch,
                                         bool radioTransmitting,
                                         bool txOwnedByUs)
{
    return latch.radioTermMasked() && radioTransmitting && !txOwnedByUs;
}

// Bookkeeping that bounds the unkey-tail mask. The caller owns the actual
// timer; this owns the episode counter that keeps a stale timer from expiring
// a newer mask (re-key during the tail starts a new episode).
struct KiwiSdrTxMaskWatchdog {
    std::uint64_t epoch{0};

    // Call on every mask-state transition. Returns true when the caller must
    // arm its timeout for the mask episode that just began.
    bool onMaskChanged(bool maskedNow)
    {
        ++epoch;
        return maskedNow;
    }

    // A timer armed at armedEpoch may expire the latch only if no newer
    // episode superseded it and the mask is still up.
    bool shouldExpire(std::uint64_t armedEpoch, bool maskedNow) const
    {
        return armedEpoch == epoch && maskedNow;
    }
};

// Hold time for "Resume audio after TX delay": how long the transmit gate
// stays closed past unkey so the resume lands on audio the Kiwi received
// AFTER the transmission ended, instead of replaying the operator's own
// delayed TX tail. Two additive components: the ingest lag (the
// receive-sync estimate measures when Kiwi audio ARRIVES relative to the
// Flex stream; without one, the sync base latency stands in as a proxy)
// plus the engine-applied presentation holdback for this source, which
// delays playback beyond arrival. The guard covers what neither can see
// (the Flex chain's own absolute latency plus websocket bunching).
constexpr int kKiwiSdrResumeHoldGuardMs = 250;
constexpr int kKiwiSdrResumeHoldMinMs = 250;
constexpr int kKiwiSdrResumeHoldMaxMs = 6000;

inline int kiwiSdrResumeHoldMs(bool estimateValid, int estimateOffsetMs,
                               int fallbackLatencyMs,
                               int appliedPresentationDelayMs)
{
    const int base = estimateValid ? std::max(estimateOffsetMs, 0)
                                   : std::max(fallbackLatencyMs, 0);
    return std::clamp(base + std::max(appliedPresentationDelayMs, 0)
                          + kKiwiSdrResumeHoldGuardMs,
                      kKiwiSdrResumeHoldMinMs, kKiwiSdrResumeHoldMaxMs);
}

// Which ingest-lag figure the hold above is entitled to use. Split out of the
// caller because it is a decision, not arithmetic: getting it wrong does not
// fail anything, it just silently changes how long the operator's audio stays
// muted. A receive-sync offset only describes the ONE profile it was measured
// against, and only while that measurement still holds the lock the sync
// engine itself demands before acting on it — everything else falls back to
// the caller's proxy.
struct KiwiSdrResumeHoldSyncInputs {
    bool isSyncTarget{false};       // this profile is the sync delay target
    bool syncEnabled{false};
    bool autoAssist{false};         // AutoAssist mode (false => Manual)
    bool estimateValid{false};
    bool estimateHeld{false};
    float estimateConfidence{0.0f};
    float autoLockConfidence{0.0f};
    int estimateOffsetMs{0};        // positive delays Flex relative to Kiwi
    int manualOffsetMs{0};
};

struct KiwiSdrResumeHoldIngestLag {
    bool valid{false};
    int offsetMs{0};
};

inline KiwiSdrResumeHoldIngestLag kiwiSdrResumeHoldIngestLag(
    const KiwiSdrResumeHoldSyncInputs& in)
{
    if (!in.isSyncTarget || !in.syncEnabled) {
        return {};
    }
    if (in.autoAssist) {
        // Same condition ReceivePresentationSync applies before it will act
        // on an estimate: held, or confident enough to lock.
        if (in.estimateValid
            && (in.estimateHeld
                || in.estimateConfidence >= in.autoLockConfidence)) {
            return {true, in.estimateOffsetMs};
        }
        return {};
    }
    // Manual: a negative or zero offset says the Kiwi is EARLIER than the
    // Flex stream, which tells us nothing about its ingest lag.
    if (in.manualOffsetMs > 0) {
        return {true, in.manualOffsetMs};
    }
    return {};
}

} // namespace AetherSDR
