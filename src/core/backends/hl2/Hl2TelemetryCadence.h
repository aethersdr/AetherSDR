#pragma once

#include <array>
#include <cstdint>   // std::uint8_t -- libstdc++ does not get it via <array>

#include <optional>

// When to poll the HL2's alternate control port for telemetry, as a pure
// function of the IQ path's state (derivation:
// docs/architecture/hl2-stream-free-telemetry.md §3). Discovery preempts EP6 in
// the gateware (usopenhpsdr1.v:234 before :238), so every poll delays IQ; EP6
// already carries these fields at 10 Hz. Poll only when EP6 is not delivering,
// and slowly. The fastest cadence is deliberately the stalled-stream case.

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
// It gates the display states only: a stalled stream is polled regardless,
// and an unwatched HeldByOther poll is traffic into another operator's session.
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
        // 1 Hz only while watched: these packets land in someone else's
        // session, and the state latches as soon as any in-use radio answers.
        return surfaceVisible ? 1000 : 0;
    case Hl2LinkState::NotConnected:
        return surfaceVisible ? 1000 : 0;
    }
    return 0;
}


// How long the packet counter must sit still before the stream is called
// stalled.
//
// A duration since the counter last advanced, not a tick-to-tick delta: the
// counter is mirrored at 1 Hz (linkCountersUpdated), so a 1 Hz tick aliases
// against it and sees false stalls (hl2_link_state_alias_test). 2500 ms is two
// kLinkPublishIntervalMs (1000) periods plus margin; a real stall is declared
// 2.5-3.5 s after it starts.
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

// Which replies Hl2TelemetryPoller::onReadyRead() may believe (socket-free).
// `latched` is the MAC of the first accepted answer; an aim names only an IP,
// so there is no caller-supplied expected MAC.
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
