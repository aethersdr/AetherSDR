#pragma once

// When a receiver-ceiling move is a CAPABILITY REVISION worth announcing — as a
// pure decision, so the guard is testable without a radio, a socket or a running
// event loop (the layer #5358 asks for, same shape as Hl2DspSetupPolicy.h).
//
// WHY THERE IS A GUARD AT ALL. Hl2Backend reports receiverCeiling() as both
// maxSlices and maxPanadapters, and that ceiling is min(board count, what the
// EP6 link budget admits at the current span) — so it FALLS when the operator
// zooms out. At 384 kHz a fourth receiver is ~89 Mbit/s on the HL2's 100BASE-T
// and genuinely cannot be delivered, so the honest limit there is 3. That is a
// real revision of the descriptor the aetherd control protocol serializes
// (#3849 step 3), and until #5594 nothing announced it.
//
// But a zoom is a DRAG. setPanBandwidth is fed roughly every 33 ms during a
// sweep and crosses several rates on the way, while the great majority of those
// rate changes leave the ceiling exactly where it was. Announcing per rate
// change rather than per CEILING change would turn one gesture into a
// republish storm at every capability consumer. Hence: compare against the last
// value actually announced, not against "did the rate move".
//
// -1 is the disconnected sentinel rather than 0 so that the first announcement
// after a connect is driven by an explicit seed() and never by a sentinel that
// happens to equal a real ceiling.

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
