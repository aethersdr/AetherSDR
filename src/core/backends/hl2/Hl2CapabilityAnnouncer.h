#pragma once

// Decides when a receiver-ceiling change is a capability revision worth
// announcing (pure, testable without a radio). receiverCeiling() is
// min(board count, EP6 link budget at the current span), so it falls on zoom-out
// (a 4th receiver at 384 kHz is ~89 Mbit/s, over 100BASE-T). A zoom drag feeds
// setPanBandwidth every ~33 ms, so announce only when the ceiling differs from
// the last value announced, never per rate change (#5594). -1 = disconnected,
// so the first post-connect value comes from an explicit seed().

namespace AetherSDR::hl2 {

class ReceiverCeilingAnnouncer {
public:
    // Record what the connect edge already published, without announcing it.
    // The connect republishes capabilities through connectionStateChanged, so
    // announcing here as well would be a duplicate — and skipping the seed
    // entirely is what would make the first zoom announce a ceiling that never
    // moved.
    void seed(int ceiling) noexcept { m_announced = ceiling; }

    // Forget it. A reconnect republishes from scratch, so the previous
    // session's announcement describes nothing.
    void reset() noexcept { m_announced = kNone; }

    // True exactly once per distinct ceiling. Stateful on purpose: the caller's
    // job is then a single `if`, with no way to emit without also recording it.
    [[nodiscard]] bool shouldAnnounce(int ceiling) noexcept
    {
        if (ceiling == m_announced)
            return false;
        m_announced = ceiling;
        return true;
    }

    [[nodiscard]] int announced() const noexcept { return m_announced; }

    static constexpr int kNone = -1;

private:
    int m_announced = kNone;
};

}  // namespace AetherSDR::hl2
