#pragma once

#include <QtGlobal>

#include <atomic>

// The DDC rate actually written to the register, kept apart from the target.
// The rate register is radio-wide, so a span change rebuilds every chain first
// (0.6-1.1 s, own thread) while Hl2Backend::m_sampleRateHz already holds the
// target. Two readers must use the committed rate, never the optimistic one:
// the rollback value for a failed build (overlapping zoom crossings), and the
// rate a receiver opened mid-build is configured for. Socket-free so
// hl2_rate_commit_test pins the ordering.
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
