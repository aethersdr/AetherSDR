#include "models/SliceLinkPolicy.h"

#include <iostream>

using namespace AetherSDR;
using SliceLinkPolicy::Action;
using SliceLinkPolicy::Decision;
using SliceLinkPolicy::Input;
using SliceLinkPolicy::PendingWrites;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

constexpr int kA = 0;
constexpr int kB = 1;

// A tiny adapter mirror: applies Decisions to link state exactly the way
// MainWindow's handler does, so multi-event sequences (echo trains, origin
// handover, suspend → resume, settle re-assert) are exercised against the
// same state transitions the real adapter performs. modelHz mirrors the
// SliceModel values (optimistic writes included) so the settle check can be
// simulated faithfully.
struct LinkSim {
    PendingWrites pending[2];
    std::int64_t modelHz[2]{0, 0};
    int originId{-1};

    Decision feed(int changedId, std::int64_t hz, std::int64_t nowMs,
                  bool peerLocked = false, bool sweep = false)
    {
        modelHz[changedId] = hz;  // the member's model already holds the value
        Input in;
        in.changedHz = hz;
        in.nowMs = nowMs;
        in.peerLocked = peerLocked;
        in.swrSweepRunning = sweep;

        const Decision d = SliceLinkPolicy::classify(in, pending[changedId]);
        if (d.consumeThrough >= 0) {
            pending[changedId].consumeThrough(d.consumeThrough);
        }
        if (d.becomeOrigin) {
            originId = changedId;
        }
        if (d.recordPeerWrite) {
            const int peer = (changedId == kA) ? kB : kA;
            pending[peer].record(hz, nowMs);
            modelHz[peer] = hz;  // adapter tunes the peer optimistically
        }
        return d;
    }

    // Mirrors SliceModel's signal order for a local request:
    // frequencyChanged first, then frequencyCommandIssued arms the origin's
    // radio-echo expectation.
    Decision command(int changedId, std::int64_t hz, std::int64_t nowMs,
                     bool peerLocked = false)
    {
        const Decision d = feed(changedId, hz, nowMs, peerLocked);
        pending[changedId].record(hz, nowMs);
        return d;
    }

    // The adapter's trailing settle check: when the pair is diverged, the
    // origin's current value re-enters the classified path.
    Decision settleCheck(std::int64_t nowMs)
    {
        if (originId < 0 || modelHz[kA] == modelHz[kB]) {
            return {Action::IgnoreSweep};  // placeholder "no action needed"
        }
        return feed(originId, modelHz[originId], nowMs);
    }
};

} // namespace

int main()
{
    bool ok = true;

    // ── Basic propagation records a pending write toward the peer. ───────
    {
        LinkSim sim;
        const Decision d = sim.feed(kA, 14'074'000, 1000);
        ok &= expect(d.action == Action::Propagate && d.recordPeerWrite
                         && d.becomeOrigin,
                     "genuine move propagates and takes the origin");
        ok &= expect(sim.pending[kB].count == 1 && sim.originId == kA,
                     "propagation arms the peer's pending-write ring");
    }

    // ── Echo train during a fast spin: every in-flight echo (which emits
    //    because the peer's model is optimistically ahead) is matched by
    //    value, in order, and never propagates backwards. ─────────────────
    {
        LinkSim sim;
        sim.feed(kA, 14'074'000, 1000);
        sim.feed(kA, 14'074'100, 1010);
        sim.feed(kA, 14'074'200, 1020);
        ok &= expect(sim.pending[kB].count == 3, "spin queues three writes");
        const Decision e1 = sim.feed(kB, 14'074'000, 1100);
        const Decision e2 = sim.feed(kB, 14'074'100, 1110);
        const Decision e3 = sim.feed(kB, 14'074'200, 1120);
        ok &= expect(e1.action == Action::IgnoreEcho
                         && e2.action == Action::IgnoreEcho
                         && e3.action == Action::IgnoreEcho,
                     "the whole echo train is classified as echoes");
        ok &= expect(sim.pending[kB].count == 0,
                     "in-order echoes drain the ring completely");
    }

    // ── Regression: the origin's own stale radio echoes must not be treated
    //    as fresh intent and replayed backwards onto the peer. Each genuine
    //    origin move arms its own echo ring as well as the peer's. ──────────
    {
        LinkSim sim;
        sim.command(kA, 14'074'000, 1000);
        sim.command(kA, 14'074'100, 1010);
        ok &= expect(sim.pending[kA].count == 2
                         && sim.pending[kB].count == 2,
                     "spin arms echo expectations on origin and peer");
        const Decision originEcho1 = sim.feed(kA, 14'074'000, 1100);
        ok &= expect(originEcho1.action == Action::IgnoreEcho
                         && sim.modelHz[kB] == 14'074'100,
                     "stale origin echo cannot retune the peer backwards");
        const Decision originEcho2 = sim.feed(kA, 14'074'100, 1110);
        ok &= expect(originEcho2.action == Action::IgnoreEcho
                         && sim.modelHz[kB] == 14'074'100,
                     "origin echo train drains without peer writes");
    }

    // ── In-order consumption with duplicate values: v, w, v echoes must
    //    match oldest-first so the middle write still matches its echo. ───
    {
        LinkSim sim;
        sim.feed(kA, 7'000'000, 1000);
        sim.feed(kA, 7'001'000, 1010);
        sim.feed(kA, 7'000'000, 1020);
        const Decision e1 = sim.feed(kB, 7'000'000, 1100);
        ok &= expect(e1.action == Action::IgnoreEcho
                         && sim.pending[kB].count == 2,
                     "duplicate echo matches the oldest entry only");
        const Decision e2 = sim.feed(kB, 7'001'000, 1110);
        const Decision e3 = sim.feed(kB, 7'000'000, 1120);
        ok &= expect(e2.action == Action::IgnoreEcho
                         && e3.action == Action::IgnoreEcho
                         && sim.pending[kB].count == 0,
                     "the later duplicate still matches its own echo");
    }

    // ── Regression (review blocker): a pending write whose echo was silent
    //    must not shadow a genuine later move to that frequency. The TTL
    //    expires the expectation, so wheeling back to the exact value after
    //    the window propagates normally. ───────────────────────────────────
    {
        LinkSim sim;
        sim.feed(kA, 14'074'000, 1000);          // arms pending[B], echo silent
        const std::int64_t later =
            1000 + SliceLinkPolicy::kPendingWriteTtlMs + 1;
        sim.feed(kB, 14'074'100, later);         // B takes over as origin
        const Decision d = sim.feed(kB, 14'074'000, later + 100);
        ok &= expect(d.action == Action::Propagate,
                     "expired pending write cannot swallow a genuine move");
    }

    // ── Regression (review major): back-to-back genuine tunes of BOTH
    //    members (the per-slice bridge-tune workflow) both propagate —
    //    nothing is dropped or reverted by a time latch. ──────────────────
    {
        LinkSim sim;
        const Decision a = sim.feed(kA, 14'000'000, 1000);
        const Decision b = sim.feed(kB, 7'000'000, 1050);
        ok &= expect(a.action == Action::Propagate
                         && b.action == Action::Propagate,
                     "immediate second tune of the other member propagates");
        ok &= expect(sim.originId == kB && sim.modelHz[kA] == 7'000'000,
                     "the second tune wins as the newest genuine intent");
    }

    // ── Residual hole + its safety net: a genuine move landing exactly on
    //    a still-pending value within the TTL is dropped as an echo, and
    //    the trailing settle check re-asserts the last genuine mover. ─────
    {
        LinkSim sim;
        sim.feed(kA, 14'074'000, 1000);   // pending[B] = {14.074}
        sim.feed(kB, 14'074'100, 1200);   // genuine, B becomes origin
        const Decision drop = sim.feed(kB, 14'074'000, 1300);
        ok &= expect(drop.action == Action::IgnoreEcho,
                     "documented residual: in-TTL return to a pending value "
                     "is dropped");
        ok &= expect(sim.modelHz[kA] == 14'074'100
                         && sim.modelHz[kB] == 14'074'000,
                     "the drop leaves the pair diverged (net's job)");
        const Decision heal = sim.settleCheck(1300 + SliceLinkPolicy::kSettleMs);
        ok &= expect(heal.action == Action::Propagate
                         && sim.modelHz[kA] == 14'074'000,
                     "settle check re-asserts the origin and converges");
    }

    // ── Lock suspend: a locked peer suspends propagation (no tune calls)
    //    but the mover still takes the origin, so the resume path knows
    //    whose value to re-converge to. ───────────────────────────────────
    {
        LinkSim sim;
        const Decision d = sim.feed(kA, 28'074'000, 1000, /*peerLocked=*/true);
        ok &= expect(d.action == Action::SuspendLocked && !d.recordPeerWrite
                         && d.becomeOrigin,
                     "locked peer suspends instead of tuning");
        ok &= expect(sim.originId == kA && sim.pending[kB].count == 0,
                     "suspend records no pending write");
        // Resume is deferred adapter-side (the settle check re-asserts the
        // origin once the radio's unlock has landed); the classification of
        // that deferred re-assert is a plain Propagate.
        const Decision resume = sim.feed(kA, 28'074'000, 1100);
        ok &= expect(resume.action == Action::Propagate,
                     "re-converge after unlock rides the normal propagate path");
    }

    // ── SWR sweep: stateless ignore, no ring/origin state disturbed. ─────
    {
        LinkSim sim;
        sim.feed(kA, 3'573'000, 1000);
        const Decision d = sim.feed(kA, 3'800'000, 1100,
                                    /*peerLocked=*/false, /*sweep=*/true);
        ok &= expect(d.action == Action::IgnoreSweep,
                     "sweep-driven events are ignored");
        ok &= expect(sim.originId == kA && sim.pending[kB].count == 1,
                     "sweep ignore leaves origin and pending state intact");
    }

    // ── PendingWrites ring mechanics. ────────────────────────────────────
    {
        PendingWrites ring;
        for (int i = 0; i < PendingWrites::kCapacity + 4; ++i) {
            ring.record(1'000'000 + i, 1000 + i);
        }
        ok &= expect(ring.count == PendingWrites::kCapacity,
                     "ring caps at capacity by evicting the oldest");
        ok &= expect(ring.matchOldest(1'000'000, 2000) < 0,
                     "an evicted write no longer matches");
        ok &= expect(ring.matchOldest(1'000'004, 2000) == 0,
                     "the survivor after eviction is the new oldest");
        ring.consumeThrough(ring.matchOldest(1'000'010, 2000));
        ok &= expect(ring.matchOldest(1'000'004, 2000) < 0
                         && ring.matchOldest(1'000'011, 2000) == 0,
                     "consumeThrough drops the match and everything older");
        ring.clear();
        ok &= expect(ring.count == 0 && ring.matchOldest(1'000'020, 2000) < 0,
                     "clear empties the ring");
    }

    // ── Expiry is respected by matching. ─────────────────────────────────
    {
        PendingWrites ring;
        ring.record(21'000'000, 1000);
        ok &= expect(ring.matchOldest(21'000'000, 1000 + 100) == 0,
                     "an unexpired write matches");
        ok &= expect(ring.matchOldest(
                         21'000'000,
                         1000 + SliceLinkPolicy::kPendingWriteTtlMs) < 0,
                     "an expired write no longer matches");
    }

    // ── Intent selection threshold. ──────────────────────────────────────
    {
        using SliceLinkPolicy::isIncrementalDelta;
        using SliceLinkPolicy::kIncrementalMaxDeltaHz;
        ok &= expect(isIncrementalDelta(14'074'000, 14'074'500),
                     "small delta maps to the incremental intent");
        ok &= expect(isIncrementalDelta(14'000'000,
                                        14'000'000 + kIncrementalMaxDeltaHz),
                     "threshold boundary is inclusive");
        ok &= expect(!isIncrementalDelta(14'000'000, 21'000'000),
                     "band jump maps to the absolute intent");
        ok &= expect(isIncrementalDelta(21'000'000, 20'999'000),
                     "delta test is symmetric (downward small step)");
    }

    // ── Engage eligibility. ──────────────────────────────────────────────
    {
        using SliceLinkPolicy::canLink;
        ok &= expect(canLink(0, 1, true, true, false, false),
                     "two owned non-diversity slices are linkable");
        ok &= expect(!canLink(0, 0, true, true, false, false),
                     "a slice cannot link to itself");
        ok &= expect(!canLink(0, 1, true, false, false, false),
                     "a foreign slice is not linkable (Multi-Flex)");
        ok &= expect(!canLink(0, 1, true, true, false, true),
                     "a diversity member is not linkable");
        ok &= expect(!canLink(-1, 1, true, true, false, false),
                     "invalid ids are not linkable");
    }

    return ok ? 0 : 1;
}
