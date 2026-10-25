#pragma once

#include <cstdint>

namespace AetherSDR {

// Loss accounting for a VITA-49 stream's 4-bit packet count that tolerates
// reordering.
//
// The radio numbers every packet of a stream, but they do not always arrive
// in that order: a short final packet of an FFT frame or waterfall tile
// routinely overtakes the full-size packets sent before it (seen on a FLEX
// across a routed LAN: 2,1,4,3,… on FFT; 5,4,1,0,2,3 on waterfall). Counting
// every out-of-order packet as an error reported 60–90 % "loss" on streams
// that were complete.
//
// A packet ahead of the newest marks the numbers it skipped as missing; a
// late packet that fills one of them is a reorder, not a loss. A missing
// number is counted lost only once it falls more than kWindow behind the
// newest (the 4-bit count cannot tell a later packet apart from that). A
// jump that cannot be a late packet is counted lost at once (so a stale
// duplicate, rare on UDP, would read as a loss).
//
// What the 4-bit count cannot tell: a gap of exactly 15 packets reads as a
// duplicate of the newest and counts 0, the only case where real loss is
// under-counted once the stream is running. In a stream's first kWindow
// packets a gap of more than kWindow cannot be told apart from packets
// reordered across the first one and may be miscounted (over or under).
// The stream's network-quality input counts breaks, one per gap, so this
// affects only the per-packet count Network Diagnostics shows.
class VitaSequenceTracker {
public:
    static constexpr int kWindow = 7;

    struct Result {
        int lost = 0;        // packets now known lost
        int breaks = 0;      // gaps they belong to that were not counted yet
        bool reordered = false;  // this packet filled a gap (arrived late)
        bool duplicate = false;
    };

    void reset()
    {
        m_last = -1;
        m_missing = 0;
        m_started = 0;
        m_lastLost = -1;
    }

    Result add(int seq)
    {
        Result r;
        seq &= 0x0F;
        if (m_last < 0) {
            m_last = seq;
            m_started = 0;
            return r;
        }
        if (m_started < kWindow) {
            ++m_started;
        }
        const int ahead = (seq - m_last) & 0x0F;
        if (ahead == 0) {
            r.duplicate = true;
            return r;
        }
        if (ahead > 16 - kWindow - 1 && (m_missing & bit(seq))) {
            // Late, and filling a gap: reordered.
            m_missing &= static_cast<uint16_t>(~bit(seq));
            r.reordered = true;
            return r;
        }
        if (ahead > 16 - kWindow - 1 && m_started < kWindow) {
            // Late, from before the stream's first packet here: nothing was
            // lost.
            r.reordered = true;
            return r;
        }
        if (ahead > 16 - kWindow - 1) {
            // Not a gap we are waiting for: a jump of more than kWindow,
            // too far to be reordering. Everything in between is lost, and
            // so is all that was still awaited.
            r.lost = (ahead - 1) + pending();
            r.breaks = r.lost > 0 ? 1 : 0;
            m_missing = 0;
            m_last = seq;
            m_lastLost = (seq - 1) & 0x0F;
            return r;
        }
        // Ahead: the numbers in between are missing for now.
        for (int i = 1; i < ahead; ++i) {
            m_missing |= bit(m_last + i);
        }
        m_last = seq;
        m_missing &= static_cast<uint16_t>(~bit(seq));
        r.lost = expire(r.breaks);
        return r;
    }

    // Numbers still awaited (not yet counted lost).
    int pending() const
    {
        int n = 0;
        for (uint16_t m = m_missing; m; m &= static_cast<uint16_t>(m - 1)) {
            ++n;
        }
        return n;
    }

private:
    static uint16_t bit(int seq) { return static_cast<uint16_t>(1u << (seq & 0x0F)); }

    // Count and drop missing numbers more than kWindow behind the newest.
    // A gap leaves the window one number per packet; numbers next to the one
    // counted before belong to the same gap, so a gap is one break.
    int expire(int& breaks)
    {
        int lost = 0;
        for (int behind = 15; behind > kWindow; --behind) {
            const int seq = (m_last - behind) & 0x0F;
            const uint16_t b = bit(seq);
            if (m_missing & b) {
                m_missing &= static_cast<uint16_t>(~b);
                ++lost;
                if (m_lastLost < 0 || seq != ((m_lastLost + 1) & 0x0F)) {
                    ++breaks;
                }
                m_lastLost = seq;
            }
        }
        return lost;
    }

    int m_last = -1;
    uint16_t m_missing = 0;
    int m_started = 0;   // packets since the first, up to kWindow
    int m_lastLost = -1; // the last number counted lost, to join a gap's packets
};

} // namespace AetherSDR
