#pragma once

#include <array>
#include <cstdint>   // std::uint8_t -- libstdc++ does not get it via <array>

#include <optional>

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


// How long the packet counter must sit still before the stream is called
// stalled.
//
// WHY A DURATION AND NOT A TICK-TO-TICK COMPARISON. The obvious rule -- "did
// rxPackets change since the last tick?" -- was the rule, and it was wrong. The
// counter it reads is mirrored from the I/O thread by linkCountersUpdated at
// 1 Hz, and the tick that read it also ran at 1 Hz. Two clocks sampling each
// other: whenever two ticks fell between two publishes, the second saw an
// unchanged counter and declared StreamStalled on a perfectly healthy stream,
// so the app polled port 1025 through its own live session. That was observed
// on hardware on 2026-09-04 and is reproduced in hl2_link_state_alias_test.
//
// It was not a tuning error, it was a category error: a tick-to-tick delta
// measures the TICK as much as the stream. Elapsed time since the counter last
// advanced measures only the stream, and gives the same answer at any tick rate
// -- including a tick rate somebody changes later without reading this comment.
//
// 2500 ms is two publish intervals plus margin. The publish period is bounded
// below by kLinkPublishIntervalMs = 1000 and runs a little over, so a healthy
// stream never reaches 2500; two consecutive missed publishes do.
//
// THE COST, stated rather than buried: a real stall is now declared 2.5-3.5 s
// after it starts instead of ~1 s. The old ~1 s was not real. It was a coin
// flip that happened to land right in the runs that were looked at, and it paid
// for its speed with false stalls on healthy streams.
inline constexpr long long kStreamStallDeclareMs = 2500;

// The link state, from what the backend can actually observe.
//
// `msSinceRxAdvanced` is the time since the mirrored EP6 packet counter last
// went up. It is meaningful only while connected; the disconnected answers do
// not consult it.
[[nodiscard]] constexpr Hl2LinkState hl2LinkStateFor(bool connected,
                                                     bool heldByOther,
                                                     long long msSinceRxAdvanced) noexcept
{
    if (!connected)
        return heldByOther ? Hl2LinkState::HeldByOther : Hl2LinkState::NotConnected;
    return msSinceRxAdvanced >= kStreamStallDeclareMs ? Hl2LinkState::StreamStalled
                                                      : Hl2LinkState::Streaming;
}

// ---- Which replies this poller may believe --------------------------------
//
// Lifted out of Hl2TelemetryPoller::onReadyRead() so the decision can be tested
// without a socket. The transport stays in the poller; the RULE lives here,
// beside the cadence rule, for the same reason: both are policy that a test
// must be able to reach directly rather than by putting datagrams on a wire.
//
// `latched` is the MAC remembered from the first accepted answer, and it is the
// ONLY MAC concept here. An earlier version also took a caller-supplied
// `expected` MAC, but nothing could supply one: an aim names an IP and the MAC
// is not knowable until something replies, so `setExpectedMac()` never had a
// production caller and the branch existed only for its own test. Two MAC
// concepts where one can be armed is how the address policy came to rest on a
// filter that was never on (#5642 review).
struct ReplyAcceptance {
    bool accept = false;
    // The MAC to remember for next time. Unset means "leave the latch alone".
    std::optional<std::array<std::uint8_t, 6>> latch;
    const char* why = "";
};

[[nodiscard]] inline ReplyAcceptance
acceptReply(bool isHermesLite2,
            const std::array<std::uint8_t, 6>& replyMac,
            const std::optional<std::array<std::uint8_t, 6>>& latched)
{
    if (!isHermesLite2)
        return {false, std::nullopt, "not a Hermes-Lite 2"};
    if (!latched)
        return {true, replyMac, "first answer at this target — latched"};
    return {replyMac == *latched, std::nullopt,
            replyMac == *latched ? "matches the latched MAC"
                                 : "the responder at this address CHANGED"};
}

}  // namespace AetherSDR::hl2
