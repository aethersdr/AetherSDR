#pragma once

#include "core/backends/anan/P2Protocol.h"

#include <algorithm>

// Send pacing for the ANAN speaker stream (DDC Audio, PC -> radio).
//
// Socket-free, Qt-free and clock-free -- elapsed time is an argument, never read
// here -- so the whole law below is checked by arithmetic without a radio.
//
// THE PROBLEM. The stream is a fixed 48 ksps, 64 frames per packet, so the
// radio's speaker FIFO drains every 1333 us whatever the host does. Two failures
// sit either side of that: send too slowly and the FIFO empties, p2app raises the
// speaker-underflow bit (High Priority status byte 30 bit 3) and the operator
// hears a click; send too quickly and the FIFO overruns, where p2app blocks on a
// full codec FIFO and the backlog lands on us.
//
// WHY NOT SLEEP 1333 us BETWEEN PACKETS. Because nothing can. A sleep is a floor,
// not a promise, so every scheduler overrun is error that accumulates
// one-directionally into an underrun; and 1333 us is below the granularity a Qt
// timer will honour, so the attempt would not even be approximately right. It
// also puts a blocking sleep on a thread that has other work.
//
// WHY NOT READ THE RADIO'S OWN FIFO DEPTH. It is published -- Speaker FIFO Depth
// is High Priority status bytes 37-38 -- and closing the loop on it looks
// obviously right. It is not usable: that packet arrives every 200 ms while RX,
// about once per 150 speaker packets, so a servo reading it corrects on
// information up to 150 packets stale. It stays the right thing to read for
// DIAGNOSIS, and byte 30 bit 3 is the honest underrun signal.
//
// WHAT THIS DOES INSTEAD: CREDIT, NOT DELAY. Integrate the FIFO level from what
// is known exactly -- each packet sent adds kSpeakerFramesPerPacket frames, and
// elapsed wall time removes them at kSpeakerSampleRateHz -- and answer one
// question: how many packets may go out right now without passing a target
// depth. A caller draining on a coarse timer then sends a small burst each tick
// and still holds the long-run rate, because the rate is set by the drain, not by
// the tick. Nothing sleeps and no timer needs sub-millisecond accuracy.
//
// The source is clock-locked to the sink -- this audio is demodulated from the
// radio's own IQ, so it is produced at 48 ksps as measured by the same crystal
// that plays it back. There is no drift to servo out over the long run, which is
// why a target depth and a burst cap are sufficient and a rate correction is not
// needed.
namespace AetherSDR::anan {

class SpeakerAudioPacer {
public:
    // Frames to keep unplayed in the radio: sixteen packets (~21 ms). Not smaller,
    // not larger, and it is also the burst cap.
    //
    // - The caller drains on a 5 ms timer (240 frames per tick), packetsToSend()
    //   releases whole 64-frame packets, and a Qt timer interval is only a floor.
    //   Sixteen packets spans ~4 nominal ticks, so truncation and a late tick are
    //   absorbed instead of starving the radio (heard as garbled speech).
    // - It is exactly one source block at the default 48 ksps DDC
    //   (WdspChannel::computeOutputBlockSize(): 512 frames at 24 kHz -> 1024 frames
    //   at the stream rate). Audio arrives a block at a time, so a target below
    //   one block grows the queue until it drops.
    // - It is the G2's whole speaker FIFO: 1024 locations on gateware 13+
    //   (saturnregisters.c DMAFIFODepths[eSpkCodecDMA]). There is no headroom
    //   above it, and 21 ms is all the host-stall tolerance this path has; on a
    //   saturated host the radio reports underflows that sound mildly crackly and
    //   self-correct (BENCH.md 9.10). Measured on a G2 (2026-09-28): median FIFO
    //   occupancy 995 of 1024, so the estimate is accurate.
    //
    // The real measurement is High Priority status bytes 37-38
    // (HighPriorityStatus::speakerFifoLevel). The reference client's ~500-frame
    // throttle is a ceiling on running ahead for a semaphore-driven sender, not a
    // budget spanning a timer interval and a source block. Because the estimate
    // floors at zero, the target alone bounds a burst; a separate cap above it
    // could never bind.
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

    // How full OUR OWN outbound queue is. A different question from the radio's
    // FIFO, and the reason both exist.
    //
    // MEASURED ON THE G2, 2026-09-27: with only the target governing releases, the
    // queue settled at 26-45 packets of a 64 cap and STAYED there -- about 46 ms of
    // audio held on this side, permanently, swinging close enough to the cap to
    // start dropping. In steady state the pacer releases exactly what time
    // consumes, which is correct for the RATE and leaves any backlog acquired
    // during the connect transient in place for the life of the session. Nominal
    // rate is break-even by definition, so nothing ever recovers it.
    //
    // WHICH MAKES PERIODIC CATCH-UP STRUCTURAL, not a symptom -- worth stating
    // because it looks like one. Break-even release cannot drain a backlog, so
    // catch-up is the ONLY mechanism that removes one, and any backlog at all
    // therefore ends in a catch-up episode. Measured 2026-09-28 with the band
    // below: the queue sawtooths across it about once a second and catch-up is
    // active on roughly a quarter of status packets, with the queue's single
    // largest mode at exactly one block. That is the band working, not flapping.
    // The 09-27 figure looked the same from outside (~26%) for an entirely
    // different reason -- see the threshold discussion below.
    //
    // While catching up, releases ignore the target: the backlog is drained as
    // fast as the queue and the burst allow. Hysteresis rather than one threshold,
    // because audio arrives in whole DSP blocks -- sixteen packets at the default
    // DDC rate -- so the queue is inherently sawtoothed and a single threshold
    // would flap in and out of catch-up on every block boundary.
    //
    // BOTH THRESHOLDS ARE IN SOURCE BLOCKS, NOT FRACTIONS OF THE QUEUE, and that
    // is the whole point of them. An earlier revision entered at a quarter of
    // capacity, which with a 64-packet queue is sixteen packets -- EXACTLY one
    // source block at the default DDC rate. A block arrives whole, so the first
    // tick after every ordinary block saw a full quarter, latched, released the
    // block with the target ignored, and unlatched on the next tick. Catch-up was
    // the steady state, the credit target governed no release at all, and the
    // hysteresis flapped on precisely the block boundary it was written to absorb.
    // Worse, the protection was gone with it: a release bounded only by what the
    // queue holds runs at four times nominal for as long as a backlog lasts, into
    // a FIFO this target already fills completely.
    //
    // So: ENTER at two blocks, because one block in the queue is what "a block
    // just arrived" looks like and cannot be evidence of a backlog; LEAVE at one,
    // because that is the queue holding nothing but the block in flight. A tie to
    // capacity would have to be re-derived every time either the queue size or the
    // DDC rate moved; a tie to the block survives both.
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
            // Bounded by the queue and by one target's worth per call, so a
            // recovery cannot itself become an unbounded burst that overruns the
            // radio's FIFO -- which is the failure this whole class exists to
            // avoid, and would be a poor way to fix the opposite one.
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

    // The catch-up band, in source blocks. kMaxCatchUpPackets IS one block -- the
    // target was sized to cover one and nothing else -- so these read as two
    // blocks and one without a second derivation of the DSP arithmetic.
    static constexpr int kCatchUpEnterPackets = 2 * kMaxCatchUpPackets;
    static constexpr int kCatchUpLeavePackets = kMaxCatchUpPackets;

    double m_fifoFrames = 0.0;
    bool m_catchingUp = false;
};

}  // namespace AetherSDR::anan
