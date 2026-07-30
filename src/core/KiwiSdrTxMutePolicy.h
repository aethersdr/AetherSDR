#pragma once

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

} // namespace AetherSDR
