#pragma once

#include "core/backends/rtl/RtlReceivePipeline.h"
#include <chrono>
#include <thread>

namespace AetherSDR::test {

// Functional fixtures advance a sample clock, independently of host scheduling.
// Observe real output publication between callbacks instead of assuming a sleep
// scheduled WDSP, or bursting overdue input after a control-side pause.
// A receiver whose input and DSP block rates are unbalanced never settles, so
// a timeout here can mean an unsupported block geometry, not a slow worker.
inline bool settleRtlDsp(rtl::RtlReceivePipeline& pipeline)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!pipeline.outputSettledForTest()) {
        if (pipeline.needsRepair() || std::chrono::steady_clock::now() >= deadline) { return false; }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return true;
}

} // namespace AetherSDR::test
