#pragma once

#include <QtGlobal>

#include <atomic>

// THE RATE THE RADIO IS RUNNING AT, kept apart from the rate we are trying to
// reach.
//
// A pan-bandwidth change on this radio is not one step. The DDC rate register is
// radio-wide, so every receiver's chain has to be rebuilt for the new rate
// before the register can be written; the build takes 0.6-1.1 s and runs on its
// own thread while the old chains keep producing audio. Hl2Backend moves its
// m_sampleRateHz optimistically the moment the zoom is accepted, because the
// snapshot it takes for the build has to describe the TARGET.
//
// That optimism is correct and it is also a trap, because for the length of the
// build there are two different answers to "what rate is this radio at" and only
// one of them is on the wire. Two bugs came from reading the wrong one:
//
//   1. The rate a FAILED build puts back was captured from the optimistic field.
//      With two crossings overlapping -- which one drag of a zoom slider
//      produces -- the second captured the first's uncommitted target, so a
//      failed second build "restored" a rate that had never been commanded.
//
//   2. A receiver opened during the build window was configured from the
//      optimistic field, so its chain was built to decimate IQ the radio was not
//      producing yet, and would not produce at all if the build then failed.
//
// This class is the one place that answers the question, and it answers it only
// with what has actually been written to the register. Hl2Backend holds exactly
// one and reads it for both decisions above, so neither can drift back onto the
// optimistic field.
//
// SOCKET-FREE BY CONSTRUCTION. The backend seam that would exercise this end to
// end needs a MetisClient and a localhost peer, and that fixture class is
// retired (tests/tests.cmake). The ordering rule is the part that was wrong, so
// the ordering rule is what is extracted here and pinned by
// hl2_rate_commit_test.
namespace AetherSDR::hl2 {

class RateCommitLedger {
public:
    explicit RateCommitLedger(int initialRateHz) : m_committed(initialRateHz) {}

    // The rate the register has actually been written with. This is what a
    // failed crossing restores to, and what a receiver opened mid-build is
    // built for.
    int committed() const { return m_committed.load(std::memory_order_acquire); }

    // Call ONLY where the register is written. Not where the rate is chosen,
    // not where the build starts, not where the build succeeds -- where the
    // command reaches MetisClient.
    void commit(int rateHz) { m_committed.store(rateHz, std::memory_order_release); }

    // Start a crossing. The returned generation identifies it for the rest of
    // its life; a crossing whose generation is no longer current has been
    // overtaken by a newer one and must install nothing and publish nothing.
    quint64 beginCrossing()
    {
        return m_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    bool isCurrent(quint64 generation) const
    {
        return generation == m_generation.load(std::memory_order_acquire);
    }

    quint64 generation() const { return m_generation.load(std::memory_order_acquire); }

private:
    // ATOMIC because the write happens on the I/O thread, in the same turn as
    // the register write, while the GUI thread reads it to open a receiver or
    // to start the next crossing.
    std::atomic<int> m_committed;
    std::atomic<quint64> m_generation {0};
};

}  // namespace AetherSDR::hl2
