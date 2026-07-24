#pragma once

#include <cstdint>
#include <cstdlib>

// Slice Link (cross-panadapter VFO link) decision logic.
//
// Pure policy: no QObject, no I/O, no model access — the MainWindow_Wiring
// adapter feeds it a snapshot of the link state on every frequencyChanged
// event from a linked member and applies the returned Decision. Frequencies
// are integer Hz so echo classification is exact equality, immune to the
// double-epsilon subtleties the models handle upstream (SliceModel keeps its
// own 1e-9 MHz change gate; by the time an event reaches this policy it is a
// real value change).
//
// Echo model. When the adapter propagates a value onto the peer, the peer's
// SliceModel is set optimistically, so the radio's equal-value status echo is
// usually SILENT (the model's change gate suppresses it) — an armed
// expectation may simply never be consumed. What does surface is the echo
// train during rapid motion: each in-flight echo of write N arrives after
// write N+1 landed optimistically, differs from the model value, and emits.
// Status arrives in order (TCP), so classification is by value against a
// TTL-bounded ring of the frequencies we recently wrote TO that member:
//
//  - event value matches an unexpired pending write → it is an echo of our
//    own propagation; consume that entry and everything older (in-order
//    delivery means older writes can no longer echo after it) and stay
//    quiet. This is what keeps a fast origin spin from propagating the
//    peer's stale echo train backwards (the #1524 problem class).
//  - no match → a genuine move (operator, bridge, CAT, or a radio-side
//    adjustment of a value we wrote — the radio is authoritative, so that
//    too propagates); forward it to the peer through the shared tune path.
//  - a locked peer suspends propagation entirely instead of hammering the
//    tune path (which would flash LOCKED feedback on every wheel click);
//    the adapter re-converges when the lock clears.
//  - while an SWR sweep drives frequencies programmatically the link stays
//    passive (stateless ignore): the sweep restores the original frequency
//    itself, so there is nothing to re-converge afterwards.
//
// Residual misclassification is possible in exactly one shape: a genuine
// move that lands exactly on a still-pending written value within the TTL
// (returning to where the writer just was — usually already convergence).
// The adapter closes that hole with a trailing settle check (kSettleMs after
// the last link event): if the pair is still diverged, the last genuine
// mover's current frequency is re-asserted through the same guarded path.

namespace AetherSDR::SliceLinkPolicy {

// Pending-write lifetime: comfortably above an echo round-trip (LAN echoes
// arrive well under 100 ms; SmartLink adds jitter) so late echoes still
// match, short enough that a value doesn't shadow genuine reuse for long.
inline constexpr std::int64_t kPendingWriteTtlMs = 1500;

// Trailing convergence check: fires this long after the most recent link
// event; re-asserts the origin when a residual drop left the pair diverged.
inline constexpr std::int64_t kSettleMs = 600;

// Propagated-delta boundary between IncrementalTune (peer pan follows via
// panFollowVfo) and AbsoluteJump (peer pan reveals/recenters). A single
// encoder/wheel event never moves 1 MHz, so incremental spins always map to
// IncrementalTune and never clear the in-flight encoder accumulators.
inline constexpr std::int64_t kIncrementalMaxDeltaHz = 1'000'000;

// Ring of frequencies recently written to one member as propagation targets,
// oldest first. Fixed capacity: 32 unechoed in-flight writes corresponds to
// ~1 s of flat-out wheel events on a 500 ms SmartLink round trip — beyond
// it the oldest expectation is evicted and its late echo would propagate one
// backward correction (converging on the next echo), the same cosmetic
// residual a fast origin spin already has.
struct PendingWrites {
    static constexpr int kCapacity = 32;

    std::int64_t hz[kCapacity] = {};
    std::int64_t expiresAtMs[kCapacity] = {};
    int first{0};
    int count{0};

    void clear()
    {
        first = 0;
        count = 0;
    }

    void record(std::int64_t valueHz, std::int64_t nowMs)
    {
        if (count == kCapacity) {
            first = (first + 1) % kCapacity;
            --count;
        }
        const int slot = (first + count) % kCapacity;
        hz[slot] = valueHz;
        expiresAtMs[slot] = nowMs + kPendingWriteTtlMs;
        ++count;
    }

    // Oldest unexpired entry equal to valueHz (echoes arrive in write order,
    // so the oldest match is the one echoing). Logical index, or -1.
    int matchOldest(std::int64_t valueHz, std::int64_t nowMs) const
    {
        for (int i = 0; i < count; ++i) {
            const int slot = (first + i) % kCapacity;
            if (expiresAtMs[slot] > nowMs && hz[slot] == valueHz) {
                return i;
            }
        }
        return -1;
    }

    // Drop the matched entry and everything older than it.
    void consumeThrough(int logicalIndex)
    {
        if (logicalIndex < 0 || logicalIndex >= count) {
            return;
        }
        first = (first + logicalIndex + 1) % kCapacity;
        count -= logicalIndex + 1;
    }
};

enum class Action {
    Propagate,      // forward the change to the peer through the tune path
    IgnoreEcho,     // echo of our own propagation landing on this member
    IgnoreSweep,    // SWR sweep owns slice frequencies; stay passive
    SuspendLocked,  // peer is VFO-locked: mark suspended, no tune calls
};

// Snapshot of one frequencyChanged event on a link member. The pending-write
// ring passed to classify() must be the CHANGED member's ring (the values we
// wrote toward it).
struct Input {
    std::int64_t changedHz{0};
    std::int64_t nowMs{0};
    bool peerLocked{false};
    bool swrSweepRunning{false};
};

struct Decision {
    Action action{Action::IgnoreSweep};
    int consumeThrough{-1};   // matched echo index for PendingWrites::consumeThrough
    bool becomeOrigin{false}; // this member is now the settle-check origin
    bool recordPeerWrite{false};  // record changedHz in the PEER's ring, then tune
};

inline Decision classify(const Input& in, const PendingWrites& pendingForChanged)
{
    if (in.swrSweepRunning) {
        return {Action::IgnoreSweep};
    }
    const int matched = pendingForChanged.matchOldest(in.changedHz, in.nowMs);
    if (matched >= 0) {
        return {Action::IgnoreEcho, matched};
    }
    // A genuine move: this member's intent drives the link now — even when
    // the peer is locked and propagation must wait for the unlock.
    if (in.peerLocked) {
        return {Action::SuspendLocked, -1, /*becomeOrigin=*/true};
    }
    return {Action::Propagate, -1, /*becomeOrigin=*/true, /*recordPeerWrite=*/true};
}

// Intent selection for the propagated tune: small deltas ride the
// incremental pan-follow path, band-sized jumps reveal/recenter the peer.
inline bool isIncrementalDelta(std::int64_t fromHz, std::int64_t toHz)
{
    return std::llabs(toHz - fromHz) <= kIncrementalMaxDeltaHz;
}

// Engage-time eligibility. Diversity members are excluded because the radio
// already locks a diversity pair's frequencies together — a client link would
// be redundant and could fight the radio's own coupling.
inline bool canLink(int aId, int bId,
                    bool aOwned, bool bOwned,
                    bool aDiversity, bool bDiversity)
{
    return aId >= 0 && bId >= 0 && aId != bId
        && aOwned && bOwned
        && !aDiversity && !bDiversity;
}

} // namespace AetherSDR::SliceLinkPolicy
