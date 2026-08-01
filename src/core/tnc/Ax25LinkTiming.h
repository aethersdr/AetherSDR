#pragma once

#include <algorithm>
#include <cmath>

// The AX.25 airtime model: the single source of truth for "how long does this
// frame take on the air", and for the link timers derived from that answer.
//
// Why this exists: every millisecond constant in the connected-mode stack was
// originally sized for 1200-baud VHF FM. At 300 baud those numbers are not
// merely conservative, they are physically impossible — the shipped T1 of 6 s
// expired roughly 0.8 s BEFORE we finished transmitting a paclen-128 I-frame,
// so every frame timed out and every HF link died at N2. See docs/HFMODEM.md §1.
//
// The rule this header enforces: any timer that interacts with the air
// interface is DERIVED from baud rate and framing, never hardcoded. It is pure
// arithmetic (no Qt, no DSP) so it can be unit-tested standalone and reused by
// the terminal, the mailbox, and the future digipeater.

namespace AetherSDR::ax25 {

// ---------------------------------------------------------------------------
// Framing constants
// ---------------------------------------------------------------------------

// TX preamble (TXDELAY): the run of leading HDLC flags that lets the far
// receiver's PLL/AGC settle before frame data. Profile-specific, and owned here
// rather than in the modem because the airtime model must read the same value
// the modulator uses — if these ever drift apart, T1 silently stops matching
// reality. AetherAx25LibmodemShim includes this header and modulates from these.
//
// HF 300 keeps a long preamble (~2.13 s). That is the single largest term in
// the HF airtime budget and is a roadmap item (docs/HFMODEM.md §6, item 11), but it
// protects the *far* end's AGC and PLL, so it cannot be shortened on our own
// authority. When it does drop, T1 tracks it automatically.
inline constexpr int kAx25Hf300PreambleFlags = 80;   // ~2.13 s at 300 baud
inline constexpr int kAx25Vhf1200PreambleFlags = 64; // ~0.43 s at 1200 baud
inline constexpr int kAx25TxPostambleFlags = 8;

// Fixed on-air overhead of an AX.25 frame, in bytes.
inline constexpr int kAx25AddressBytes = 14;   // dest + src
inline constexpr int kAx25DigiHopBytes = 7;    // per digipeater in the path
inline constexpr int kAx25ControlBytes = 1;
inline constexpr int kAx25PidBytes = 1;        // information frames only
inline constexpr int kAx25FcsBytes = 2;

// Bit-stuffing allowance. HDLC inserts a 0 after five consecutive 1 bits;
// worst case is +20%, but real AX.25 traffic averages close to 2%. The model is
// used to size timers with a 1.5x safety factor on top, so the average is the
// right choice — the worst case would inflate every timer for no benefit.
inline constexpr double kAx25BitStuffingFactor = 1.02;

inline constexpr int kAx25BitsPerFlag = 8;
inline constexpr int kAx25BitsPerByte = 8;

// ---------------------------------------------------------------------------
// Profile
// ---------------------------------------------------------------------------

// Everything the airtime model needs that is not per-frame.
struct LinkTimingProfile {
    int baud{1200};
    int preambleFlags{kAx25Vhf1200PreambleFlags};
    int postambleFlags{kAx25TxPostambleFlags};
    int digiHops{0};
    // Our own keying overhead around the audio: PTT lead + backend settle +
    // TX tail. Dead air we pay for on every single transmission.
    int localTxOverheadMs{500};
    // How long the peer takes to turn its radio around and start answering.
    // We cannot measure this before the first exchange, so it is an allowance;
    // the RTT instrumentation in Ax25Connection reports what it really was.
    int peerTurnaroundMs{500};

    // Defaults matching the modem profile that runs at this baud rate.
    static LinkTimingProfile forBaud(int baud)
    {
        LinkTimingProfile profile;
        profile.baud = baud > 0 ? baud : 1200;
        profile.preambleFlags = profile.baud <= 300 ? kAx25Hf300PreambleFlags
                                                    : kAx25Vhf1200PreambleFlags;
        return profile;
    }
};

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

// On-air size of one frame in bytes, including FCS. `infoBytes < 0` means a
// frame with no information field (supervisory / unnumbered) — those carry no
// PID either, which is why the two cannot be collapsed into one argument.
inline int frameOnAirBytes(int infoBytes, int digiHops)
{
    int bytes = kAx25AddressBytes
        + kAx25DigiHopBytes * std::max(0, digiHops)
        + kAx25ControlBytes
        + kAx25FcsBytes;
    if (infoBytes >= 0)
        bytes += kAx25PidBytes + infoBytes;
    return bytes;
}

// Time to key, send, and unkey one frame, in milliseconds — preamble and
// postamble included, PTT overhead NOT included (callers add that separately,
// because the peer's overhead differs from ours).
inline int frameAirtimeMs(const LinkTimingProfile& profile, int infoBytes)
{
    if (profile.baud <= 0)
        return 0;
    const double dataBits = frameOnAirBytes(infoBytes, profile.digiHops)
        * kAx25BitsPerByte * kAx25BitStuffingFactor;
    const double flagBits =
        static_cast<double>(profile.preambleFlags + profile.postambleFlags) * kAx25BitsPerFlag;
    return static_cast<int>(std::lround((dataBits + flagBits) * 1000.0 / profile.baud));
}

// The modelled minimum round trip for one polled I-frame: our frame out, the
// peer's turnaround, its supervisory reply back.
inline int roundTripMs(const LinkTimingProfile& profile, int paclen)
{
    const int ourFrameMs = frameAirtimeMs(profile, std::max(0, paclen)) + profile.localTxOverheadMs;
    // We have no way to know the peer's preamble, so assume it is framing its
    // reply the way we frame ours. On a symmetric AetherSDR-to-AetherSDR link
    // that is exact; against a hardware TNC with a shorter TXDELAY it is
    // conservative, which is the safe direction for a timeout.
    const int peerReplyMs = frameAirtimeMs(profile, -1) + profile.peerTurnaroundMs;
    return ourFrameMs + peerReplyMs;
}

// Retransmission timer T1. AX.25 wants roughly twice the time to send a
// maximum frame and receive the response; 1.5x the modelled round trip lands in
// the same place while leaving room for one slow turnaround rather than two.
//
// Sanity check that this model is trustworthy: at VHF 1200 / paclen 128 it
// returns ~4.6 s, reproducing from first principles the empirically-tuned 6 s
// that has worked on 2 m for a year. That is why its 12.6 s answer for
// HF 300 / paclen 64 should be believed.
inline constexpr int kAx25MinT1Ms = 2000;
inline constexpr int kAx25MaxT1Ms = 60000;

inline int recommendedT1Ms(const LinkTimingProfile& profile, int paclen)
{
    const int t1 = static_cast<int>(std::lround(roundTripMs(profile, paclen) * 1.5));
    return std::clamp(t1, kAx25MinT1Ms, kAx25MaxT1Ms);
}

// Idle-link poll timer T3: how long a connected link may sit silent before we
// check the peer is still there. Must be comfortably longer than T1 (a poll
// while frames are in flight is pointless) but short enough to notice a dead
// peer within a couple of minutes. Long enough not to be chatty on a shared
// channel, which matters more on HF than on VHF.
inline constexpr int kAx25MinT3Ms = 60000;
inline constexpr int kAx25MaxT3Ms = 300000;

inline int recommendedT3Ms(int t1Ms)
{
    return std::clamp(t1Ms * 10, kAx25MinT3Ms, kAx25MaxT3Ms);
}

// Deferred-acknowledgement timer T2 (the half-duplex burst guard). Must be
// shorter than T1 or we would retransmit before ever sending the ack; a third
// of T1 leaves room for the peer to finish a burst without stalling it.
inline int recommendedT2Ms(int t1Ms)
{
    return std::clamp(t1Ms / 3, 500, 10000);
}

// Maximum information bytes per I-frame. AX.25 has no partial-frame recovery:
// a single bit error destroys the whole frame and costs a full retransmission.
// At 300 baud a paclen-128 frame is 4.0 s of continuous air — a coin flip on a
// QSB-prone HF path — so HF halves it. VHF FM is either solid or absent and
// keeps the larger, more efficient frame.
inline int recommendedPaclen(int baud)
{
    return baud > 0 && baud <= 300 ? 64 : 128;
}

} // namespace AetherSDR::ax25
