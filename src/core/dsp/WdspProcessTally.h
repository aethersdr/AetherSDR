#pragma once

#include "core/dsp/WdspChannel.h"

#include <atomic>
#include <cstdint>

// One counter per WdspChannel::ProcessResult outcome, for the receive stages
// that drive processIq() from an I/O thread and are read from the GUI thread.
//
// WHY THIS EXISTS. WdspChannel::processIq() distinguishes six outcomes and
// goes to real trouble to do it — a whole allocation-sequence guard around
// fexchange2 exists only to produce AllocationViolation. Both consumers
// (Hl2RxDsp::processIqBlock, AnanRxDsp::processIqBlock) then collapsed all
// five non-Ok outcomes into one unannotated `continue`, so the detection
// machinery was discarded one frame above the code that built it. A radio that
// had stopped producing audio because WDSP was returning EngineError on every
// block was indistinguishable, from outside, from a radio whose pipeline was
// still filling.
//
// WHY A SHARED TYPE RATHER THAN A COUNTER IN EACH STAGE. Hl2RxDsp and
// AnanRxDsp are two copies of the same stage and have already diverged in six
// places; each fix applied to one and not the other adds a seventh. The
// counting rule — which outcomes are faults, which one is normal, what each is
// called — is the part that must not drift between them, so it lives once.
//
// UNDERRUN IS NOT A FAULT and is deliberately not summed with the others.
// fexchange2 returns -2 whenever the asynchronous output side has nothing
// ready yet, which is the NORMAL state for the first blocks through a freshly
// opened channel and, with blockForOutput false, an ordinary occurrence
// whenever the input side runs ahead. Folding it into a fault total would put
// a large non-zero number in front of a reader on every healthy connect and
// train them to ignore the row — which is the failure mode this counter
// exists to prevent, arrived at from the other direction.
//
// RELAXED ATOMICS, for the same reason Hl2RxDsp's m_adcPeakDbfs is atomic:
// these are written on the DSP thread and read by a poll from the GUI thread.
// They are independent monotonic scalars and nothing is ordered against them,
// so a snapshot() taken across a concurrent record() may miss the very last
// increment. That is the correct trade — the alternative is a lock on the
// real-time path to make a display row one block fresher.
//
// COST ON THE REAL-TIME PATH: one relaxed fetch_add per processed block, at
// 47 blocks/second/receiver at 48 kHz. No allocation, no lock, no syscall.
class WdspProcessTally final
{
public:
    // A consistent-enough read of all six counters. Plain integers, not
    // atomics, so a caller can do arithmetic on a single observation instead
    // of re-reading members that may move between reads.
    struct Counts {
        std::uint64_t ok = 0;
        std::uint64_t underrun = 0;
        std::uint64_t busy = 0;
        std::uint64_t invalidBuffer = 0;
        std::uint64_t allocationViolation = 0;
        std::uint64_t engineError = 0;

        // The four outcomes that mean something is WRONG. Underrun is not
        // among them; see the note above.
        [[nodiscard]] std::uint64_t faults() const noexcept
        {
            return busy + invalidBuffer + allocationViolation + engineError;
        }
        // Every block handed to processIq(), fault or not. This is the
        // denominator: "3 faults" and "3 faults in 2 blocks" are different
        // reports, and only the second one is actionable.
        [[nodiscard]] std::uint64_t blocks() const noexcept
        {
            return ok + underrun + faults();
        }
    };

    // Count one outcome and return the NEW value of its own counter.
    //
    // Returning the per-kind count is what makes bounded logging possible at
    // the call site without a second lookup: `shouldLog(record(res))` fires on
    // the first occurrence of each kind and then progressively less often.
    std::uint64_t record(WdspChannel::ProcessResult result) noexcept
    {
        // NO `default:` LABEL, deliberately. A seventh ProcessResult must
        // break this build rather than be silently absorbed into a bucket —
        // being silently absorbed is the exact defect this class was written
        // to remove, and a `default:` would reintroduce it one level up.
        switch (result) {
        case WdspChannel::ProcessResult::Ok:
            return m_ok.fetch_add(1, std::memory_order_relaxed) + 1;
        case WdspChannel::ProcessResult::Underrun:
            return m_underrun.fetch_add(1, std::memory_order_relaxed) + 1;
        case WdspChannel::ProcessResult::Busy:
            return m_busy.fetch_add(1, std::memory_order_relaxed) + 1;
        case WdspChannel::ProcessResult::InvalidBuffer:
            return m_invalidBuffer.fetch_add(1, std::memory_order_relaxed) + 1;
        case WdspChannel::ProcessResult::AllocationViolation:
            return m_allocationViolation.fetch_add(1, std::memory_order_relaxed) + 1;
        case WdspChannel::ProcessResult::EngineError:
            return m_engineError.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        // Only reachable for a value that is not a valid enumerator, which is
        // already undefined behaviour upstream. Counted nowhere rather than
        // counted wrong.
        return 0;
    }

    [[nodiscard]] Counts snapshot() const noexcept
    {
        Counts c;
        c.ok = m_ok.load(std::memory_order_relaxed);
        c.underrun = m_underrun.load(std::memory_order_relaxed);
        c.busy = m_busy.load(std::memory_order_relaxed);
        c.invalidBuffer = m_invalidBuffer.load(std::memory_order_relaxed);
        c.allocationViolation = m_allocationViolation.load(std::memory_order_relaxed);
        c.engineError = m_engineError.load(std::memory_order_relaxed);
        return c;
    }

    // True when `n` is 1, 2, 4, 8, 16 ... — the logging schedule.
    //
    // A fault that repeats does so at the block rate: an unconditional warning
    // would put 47 lines a second on the same thread that paces EP2, which is
    // the H5 hazard, and would bury the first occurrence — the one line that
    // actually says when it started. Logging on powers of two keeps the first
    // occurrence of every kind, keeps the fact that it is RECURRING visible,
    // and bounds the whole run to log2(N) lines. The counter carries the
    // magnitude; the log carries the timing.
    [[nodiscard]] static constexpr bool shouldLog(std::uint64_t n) noexcept
    {
        return n != 0 && (n & (n - 1)) == 0;
    }

    // What each outcome is CALLED, in a log line or a health row. Kept here
    // rather than at two call sites so the HL2 and ANAN logs say the same
    // words about the same condition — a reader comparing the two backends
    // should not have to translate.
    [[nodiscard]] static const char* name(WdspChannel::ProcessResult result) noexcept
    {
        switch (result) {
        case WdspChannel::ProcessResult::Ok:
            return "ok";
        case WdspChannel::ProcessResult::Underrun:
            return "underrun";
        case WdspChannel::ProcessResult::Busy:
            return "busy (control operation in flight)";
        case WdspChannel::ProcessResult::InvalidBuffer:
            return "invalid buffer geometry";
        case WdspChannel::ProcessResult::AllocationViolation:
            return "allocation on the real-time path";
        case WdspChannel::ProcessResult::EngineError:
            return "WDSP engine error";
        }
        return "unknown";
    }

private:
    std::atomic<std::uint64_t> m_ok {0};
    std::atomic<std::uint64_t> m_underrun {0};
    std::atomic<std::uint64_t> m_busy {0};
    std::atomic<std::uint64_t> m_invalidBuffer {0};
    std::atomic<std::uint64_t> m_allocationViolation {0};
    std::atomic<std::uint64_t> m_engineError {0};
};
