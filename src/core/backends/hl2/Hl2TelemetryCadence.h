#pragma once

// When to poll the HL2's alternate control port for telemetry, as a pure
// function of what the IQ path is doing.
//
// This is a header rather than a member of the poller for the reason
// Hl2TxLevelPolicy.h gives about its own arithmetic: the suite must exercise the
// SAME expression the poller runs, because a test against a re-typed copy of a
// table proves only that two copies agree. It also means the rule can be tested
// without a socket, a radio, or a Qt event loop — the rule is the part with
// judgement in it; the plumbing around it is not.
//
// The full derivation is docs/architecture/hl2-stream-free-telemetry.md §3.
// The short version, because a cadence that looks arbitrary invites someone to
// "tune" it later:
//
//   * Discovery PREEMPTS the IQ path. In the gateware's transmit state machine
//     the discovery branch is tested before the EP6 branch (usopenhpsdr1.v:234
//     ahead of :238), so every poll inserts a datagram ahead of a queued IQ
//     packet. Polling is not free, and it is least free exactly when a stream
//     is running.
//   * The radio's own refresh rate for these fields is NOT ESTABLISHED. The
//     bench could only bound it as "consistent with anything from ~10 Hz
//     upward" — with no RF the ADC fields dither across 3-4 codes, so timing
//     their changes measures the dither rather than the refresh. Above that
//     rate a poll returns the same reading: cost with no information.
//   * The in-band EP6 path already publishes every one of these fields at
//     10 Hz, for free, whenever it is running.
//
// So the rule is not "how fast can we poll" (answered: ~86 Hz, measured, and
// irrelevant). It is: poll only when the in-band path is NOT delivering, and
// poll slowly.
//
// Note which case is fastest below. The highest cadence is the FAILURE case,
// not the healthy one. That inversion is the whole point of the feature — the
// telemetry rides the very packets whose absence is the fault being diagnosed,
// so an instrument that reads fastest when things are fine is reading fastest
// when it is needed least.

namespace AetherSDR::hl2 {

// What the IQ path is doing. The poller derives its cadence from this rather
// than taking an interval, so the rule lives in one place instead of in
// whichever caller set a timer last.
enum class Hl2LinkState {
    NotConnected,    // no session
    Streaming,       // we hold the stream and EP6 is arriving
    StreamStalled,   // we hold the stream and EP6 has stopped
    HeldByOther,     // discovery says in-use, and it is not us
};

// Milliseconds between polls, or 0 for "do not poll at all".
//
// `surfaceVisible` is whether anything is actually reading the telemetry.
//
// It gates the two DISPLAY states and neither of the fault states. Polling a
// radio nobody is looking at is pure wire cost, and in HeldByOther those
// packets land in another operator's session, which makes an unwatched poll
// there worse than merely wasteful. A stalled stream is the opposite case: it
// is diagnosed whether or not a panel is open, because the reason to poll then
// is the fault and not the panel.
[[nodiscard]] constexpr int hl2PollIntervalMs(Hl2LinkState state,
                                              bool surfaceVisible) noexcept
{
    switch (state) {
    case Hl2LinkState::Streaming:
        // The in-band path is already delivering these exact fields, in these
        // exact units, at 10 Hz. A poll here buys nothing and costs an IQ slot.
        return 0;
    case Hl2LinkState::StreamStalled:
        // 2 Hz. The case the feature exists for: the in-band path has gone
        // silent and cannot report its own silence.
        return 500;
    case Hl2LinkState::HeldByOther:
        // 1 Hz while something is reading, silent otherwise. A status display,
        // not a meter — and every one of these packets lands in somebody
        // else's session, so an unwatched poll here is not just wasted, it is
        // traffic aimed at an operator who did not ask for it.
        //
        // This was unconditional when the rule was first written, and wiring it
        // up showed why that was wrong: the state latches on as soon as any
        // in-use radio answers, so the app would have polled a stranger's
        // session forever with nothing on screen.
        return surfaceVisible ? 1000 : 0;
    case Hl2LinkState::NotConnected:
        return surfaceVisible ? 1000 : 0;
    }
    return 0;
}

}  // namespace AetherSDR::hl2
