#pragma once

#include <cstdint>
#include <utility>

namespace AetherSDR::hl2 {

enum class IoBoardAction { Send, Coalesce, DropDisconnected };

// The radio already moves its TX NCO and filter on a band change, including
// during MOX/TUNE. Deferring only the amplifier until unkey strands it on the
// old band. This schedule follows the radio; it does not sequence RF/relays.
[[nodiscard]] constexpr IoBoardAction ioBoardAction(bool connected,
                                                    bool throttleActive,
                                                    bool bandChanged) noexcept
{
    if (!connected) {
        return IoBoardAction::DropDisconnected;
    }
    if (throttleActive && !bandChanged) {
        return IoBoardAction::Coalesce;
    }
    return IoBoardAction::Send;
}

// Pending work belongs to the current scheduling window. An immediate band
// change supersedes a same-band value waiting in the previous window.
class IoBoardSchedule {
public:
    [[nodiscard]] IoBoardAction request(bool connected, bool throttleActive,
                                        bool bandChanged, std::uint64_t hz) noexcept
    {
        const IoBoardAction action = ioBoardAction(connected, throttleActive, bandChanged);
        m_pendingHz = action == IoBoardAction::Coalesce ? hz : 0;
        return action;
    }

    [[nodiscard]] std::uint64_t takePending() noexcept
    {
        return std::exchange(m_pendingHz, 0);
    }

    void reset() noexcept { m_pendingHz = 0; }

private:
    std::uint64_t m_pendingHz = 0;
};

} // namespace AetherSDR::hl2
