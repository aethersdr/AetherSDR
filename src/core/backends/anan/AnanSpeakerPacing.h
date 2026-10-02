#pragma once

#include "core/backends/anan/P2Protocol.h"

#include <algorithm>

// Send pacing for the ANAN speaker stream (DDC Audio, PC -> radio). Socket-,
// Qt- and clock-free: elapsed time is an argument, so the law is testable.
// The stream is fixed 48 ksps, 64 frames/packet, so the radio FIFO drains one
// packet per 1333 us. Too slow underflows (High Priority status byte 30 bit 3, a
// click); too fast overruns and p2app blocks. Sleeping 1333 us cannot work (below
// Qt timer granularity, and overruns accumulate), and the radio's FIFO depth
// (status bytes 37-38) arrives only every 200 ms, ~150 packets stale -- fine for
// diagnosis, useless for control. So this integrates a CREDIT estimate: +64
// frames per packet sent, -48000 frames/s elapsed, and answers how many packets
// may go now without passing the target. Source and sink share the radio's
// crystal, so there is no long-run drift to servo.
namespace AetherSDR::anan {

class SpeakerAudioPacer {
public:
    // Frames to keep unplayed in the radio: sixteen packets (~21 ms), also the burst
    // cap. It spans ~4 ticks of the caller's 5 ms drain timer (absorbs late ticks);
    // it is one source block at the default 48 ksps DDC (512 frames at 24 kHz ->
    // 1024 at the stream rate, WdspChannel::computeOutputBlockSize()), so a smaller
    // target grows the queue; and it is the G2's whole speaker FIFO (1024 on
    // gateware 13+, saturnregisters.c DMAFIFODepths[eSpkCodecDMA]). Measured G2
    // median occupancy: 995/1024. The estimate floors at zero, so the target alone
    // bounds a burst.
    static constexpr double kTargetFifoFrames = 16.0 * kSpeakerFramesPerPacket;

    [[nodiscard]] double estimatedFifoFrames() const noexcept { return m_fifoFrames; }

    // Time passed. Frames drain at the stream's fixed rate, and the estimate
    // floors at empty: a long stall must not bank negative depth, which would
    // then license a burst far past the target as "catching up".
    void advance(double elapsedSeconds) noexcept
    {
        if (elapsedSeconds > 0.0) {
            m_fifoFrames -= elapsedSeconds * static_cast<double>(kSpeakerSampleRateHz);
        }
        if (m_fifoFrames < 0.0) {
            m_fifoFrames = 0.0;
        }
    }

    // How full OUR OWN outbound queue is (distinct from the radio FIFO). In steady
    // state the pacer releases exactly what time consumes, so a backlog from the
    // connect transient never drains on its own; catch-up is the only mechanism that
    // removes one, and periodic catch-up (~once a second) is normal. While catching
    // up, releases ignore the target, bounded by kMaxCatchUpPackets. Thresholds are
    // in SOURCE BLOCKS (audio arrives a whole block at a time), not queue fractions:
    // ENTER at two blocks, since one queued block is just "a block arrived"; LEAVE at
    // one, the block in flight. A quarter-of-capacity threshold equals one block at a
    // 64-packet queue and would make catch-up the steady state.
    void setBacklog(int packetsQueued, int capacityPackets) noexcept
    {
        if (capacityPackets <= 0) {
            return;
        }
        // A queue too small to hold the band still has to be able to catch up, so
        // clamp rather than letting the thresholds invert or become unreachable.
        const int enterAt = std::min(kCatchUpEnterPackets, capacityPackets);
        const int leaveAt = std::min(kCatchUpLeavePackets, enterAt / 2);
        if (!m_catchingUp && packetsQueued >= enterAt) {
            m_catchingUp = true;
        } else if (m_catchingUp && packetsQueued <= leaveAt) {
            m_catchingUp = false;
        }
    }

    [[nodiscard]] bool catchingUp() const noexcept { return m_catchingUp; }

    // How many packets may go out now, given how many are waiting.
    [[nodiscard]] int packetsToSend(int packetsQueued) const noexcept
    {
        if (packetsQueued <= 0) {
            return 0;
        }
        if (m_catchingUp) {
            // Capped per call so recovery cannot overrun the radio FIFO.
            return std::min(packetsQueued, kMaxCatchUpPackets);
        }
        const double room = kTargetFifoFrames - m_fifoFrames;
        if (room <= 0.0) {
            return 0;
        }
        const int byRoom = static_cast<int>(room / kSpeakerFramesPerPacket);
        return std::min(packetsQueued, byRoom);
    }

    // Call once per packet ACTUALLY sent, never per packet built: a packet that
    // failed to send never reached the FIFO, and counting it would read a broken
    // link as a comfortably full buffer and stop sending for good.
    void onPacketSent() noexcept
    {
        m_fifoFrames += static_cast<double>(kSpeakerFramesPerPacket);
    }

    // Between streams -- a reconnect, or a rate change that rebuilds the DSP.
    // The radio's FIFO does not survive those either, so carrying an estimate
    // across one would start the next stream believing it was already fed.
    void reset() noexcept
    {
        m_fifoFrames = 0.0;
        m_catchingUp = false;
    }

private:
    // One target's worth per drain call while catching up. Enough to make real
    // progress against a backlog at every tick, bounded so the recovery cannot
    // overshoot into the overrun this class exists to prevent.
    static constexpr int kMaxCatchUpPackets =
        static_cast<int>(kTargetFifoFrames / kSpeakerFramesPerPacket);

    // kMaxCatchUpPackets is one source block, so these are two blocks and one.
    static constexpr int kCatchUpEnterPackets = 2 * kMaxCatchUpPackets;
    static constexpr int kCatchUpLeavePackets = kMaxCatchUpPackets;

    double m_fifoFrames = 0.0;
    bool m_catchingUp = false;
};

}  // namespace AetherSDR::anan
