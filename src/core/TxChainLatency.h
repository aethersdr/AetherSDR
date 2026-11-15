#pragma once

#include <atomic>

// TxVoiceProcessor::latencyFrames() (48 kHz frames), published from the audio
// thread for MetisClient's once-per-over key-down log on the HL2 I/O thread
// (#6052). One relaxed atomic, no seam signal. -1: nothing published, or the
// transmit audio does not pass through the voice processor (DAX/TCI).
namespace AetherSDR::TxChainLatency {

inline std::atomic<int> g_voiceProcessorFrames{-1};

inline void publishVoiceProcessorFrames(int frames) noexcept
{
    g_voiceProcessorFrames.store(frames, std::memory_order_relaxed);
}

[[nodiscard]] inline int voiceProcessorFrames() noexcept
{
    return g_voiceProcessorFrames.load(std::memory_order_relaxed);
}

}  // namespace AetherSDR::TxChainLatency
