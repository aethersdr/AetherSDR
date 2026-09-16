// Pins the stream-free telemetry poll cadence (roadmap item #15).
//
// The cadence is the only part of the poller with a judgement in it, and it is
// the part that will look arbitrary to the next reader — which is exactly the
// kind of constant that gets "tuned" into something worse. So it is a pure
// function in a header and this test exercises that function, not a copy of the
// table (Hl2TxLevelPolicy.h's rule).
//
// The derivation is docs/architecture/hl2-stream-free-telemetry.md §3. What
// this test defends, in order of how easily each could be lost:
//
//   1. Streaming polls at ZERO. Not "rarely" — never. The in-band EP6 path
//      already carries these fields at 10 Hz, and every poll preempts an IQ
//      packet in the gateware's transmit state machine (usopenhpsdr1.v:234 is
//      tested ahead of :238). A well-meaning "keep it fresh" edit here is a
//      regression, not an improvement.
//   2. The stalled case is FASTER than the healthy case. If a future edit ever
//      makes Streaming poll faster than StreamStalled, the instrument reads
//      fastest when it is needed least, and the feature has been inverted.
//   3. Nothing exceeds 10 Hz. The radio's refresh rate is not established and
//      is only bounded as "~10 Hz or faster", so a shorter interval spends IQ
//      slots on readings that may not have regenerated.
//   4. surfaceVisible gates the two DISPLAY states and NEITHER fault state. A
//      fault is diagnosed whether or not anyone has a panel open; a display
//      that nobody is reading should not be generating traffic — least of all
//      in HeldByOther, where the packets land in another operator's session.
//
// Pure header, no Qt, no socket, no radio.

#include "core/backends/hl2/Hl2TelemetryCadence.h"

// Included here rather than reached through the header under test.
//
// All three arrive today only because Hl2TelemetryCadence.h needs them for its
// OWN declarations. That is a coincidence of the header's current shape, not a
// contract: the day acceptReply() stops taking a std::array or its optional
// parameters change form, the header drops the include it no longer needs and
// this file stops compiling -- reporting it as a failure of the thing it tests.
// (The header could not drop these while still needing them for its own
// signature; what this buys is independence from a future change to WHAT it
// needs.)
#include <array>
#include <cstdint>
#include <optional>

#include <cstdio>
#include <initializer_list>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main()
{
    constexpr bool kVisible = true;
    constexpr bool kHidden = false;

    // ---- 1. A healthy stream is not polled at all ----
    check(hl2PollIntervalMs(Hl2LinkState::Streaming, kVisible) == 0,
          "streaming: no poll, even with the panel open — EP6 already carries it");
    check(hl2PollIntervalMs(Hl2LinkState::Streaming, kHidden) == 0,
          "streaming: no poll with the panel closed either");

    // ---- 2. The failure case is the fast one ----
    const int stalled = hl2PollIntervalMs(Hl2LinkState::StreamStalled, kHidden);
    // Visible, because held-by-other is demand-gated: comparing against its
    // silent value would compare a cadence with "not polling at all", which is
    // not the ordering this invariant is about.
    const int held    = hl2PollIntervalMs(Hl2LinkState::HeldByOther, kVisible);
    const int idle    = hl2PollIntervalMs(Hl2LinkState::NotConnected, kVisible);

    check(stalled > 0, "a stalled stream IS polled — nothing else can report its silence");
    check(stalled < held,
          "stalled polls FASTER than held-by-other: the diagnostic case outranks the display");
    check(hl2PollIntervalMs(Hl2LinkState::Streaming, kVisible) == 0 && stalled > 0,
          "the fastest cadence is a failure state, not the healthy one");

    // A stalled stream is diagnosed whether or not anyone has the panel open.
    // The reason to poll then is the fault, not the panel.
    check(hl2PollIntervalMs(Hl2LinkState::StreamStalled, kHidden)
              == hl2PollIntervalMs(Hl2LinkState::StreamStalled, kVisible),
          "a stalled stream is polled regardless of whether a surface is visible");
    // CHANGED after the wiring went in, and the reason is worth keeping.
    // HeldByOther was unconditional here at first, on the reasoning that a
    // status display should keep updating. Connecting it showed the flaw: the
    // state latches on the moment any in-use radio answers, so an app sitting
    // idle would have polled a stranger's session forever with nothing on
    // screen. These packets land in another operator's session, which makes an
    // unwatched poll here worse than merely wasteful.
    check(hl2PollIntervalMs(Hl2LinkState::HeldByOther, kHidden) == 0,
          "held-by-other with nothing reading: silent — those packets land in "
          "someone else's session");
    check(hl2PollIntervalMs(Hl2LinkState::HeldByOther, kVisible) > 0,
          "held-by-other with something reading: polled");

    // ---- 3. Nothing polls faster than the radio is known to refresh ----
    // 100 ms is 10 Hz, the LOWEST rate the bench could not exclude. Anything
    // shorter is asking for a number the radio may not have regenerated, at the
    // cost of an IQ packet slot.
    for (const auto s : {Hl2LinkState::NotConnected, Hl2LinkState::Streaming,
                         Hl2LinkState::StreamStalled, Hl2LinkState::HeldByOther}) {
        for (const bool vis : {false, true}) {
            const int ms = hl2PollIntervalMs(s, vis);
            check(ms == 0 || ms >= 100,
                  "no state polls faster than 10 Hz — the refresh rate is unestablished");
        }
    }

    // ---- 4. surfaceVisible gates the display states, not the fault states ----
    check(idle > 0, "idle with a visible surface: polled");
    check(hl2PollIntervalMs(Hl2LinkState::NotConnected, kHidden) == 0,
          "idle with nothing watching: not polled at all");

    // ---- the table itself, so a silent change to any value is caught ----
    check(hl2PollIntervalMs(Hl2LinkState::Streaming, true) == 0,     "Streaming      -> 0 ms");
    check(hl2PollIntervalMs(Hl2LinkState::StreamStalled, false) == 500, "StreamStalled  -> 500 ms");
    check(hl2PollIntervalMs(Hl2LinkState::HeldByOther, true) == 1000,  "HeldByOther+   -> 1000 ms");
    check(hl2PollIntervalMs(Hl2LinkState::HeldByOther, false) == 0,     "HeldByOther-   -> 0 ms");
    check(hl2PollIntervalMs(Hl2LinkState::NotConnected, true) == 1000, "NotConnected+  -> 1000 ms");
    check(hl2PollIntervalMs(Hl2LinkState::NotConnected, false) == 0,   "NotConnected-  -> 0 ms");

    // The rule is constexpr, so the table is fixed at compile time and a
    // consumer can branch on it without a runtime call.
    static_assert(hl2PollIntervalMs(Hl2LinkState::Streaming, true) == 0,
                  "streaming must be compile-time zero");
    static_assert(hl2PollIntervalMs(Hl2LinkState::StreamStalled, false)
                      < hl2PollIntervalMs(Hl2LinkState::HeldByOther, true),
                  "the failure case must be the faster one, at compile time");
    // A fault is diagnosed whether or not anyone is looking; a display is not.
    // This pins WHICH states are demand-gated, so a later edit cannot quietly
    // make the stalled case wait for a panel to be open.
    static_assert(hl2PollIntervalMs(Hl2LinkState::StreamStalled, false) > 0,
                  "a stalled stream is polled with nothing on screen");
    static_assert(hl2PollIntervalMs(Hl2LinkState::HeldByOther, false) == 0,
                  "a display state is not polled with nothing on screen");

    // ---- which replies the poller may believe ----------------------------
    //
    // acceptReply() is the rule lifted out of Hl2TelemetryPoller::onReadyRead()
    // so it can be exercised without a socket. The defect it answers:
    // setExpectedMac() had no production caller, so the MAC filter it gated was
    // dead on every live path while the address policy rested on it in the
    // comments (#5642 review). That setter and its branch are now gone; the
    // latch below is the only MAC concept, because it is the only one that can
    // be armed.
    {
        const std::array<std::uint8_t, 6> radioA{{0x00, 0x1C, 0xC0, 0xA2, 0x13, 0xDD}};
        const std::array<std::uint8_t, 6> radioB{{0x00, 0x1C, 0xC0, 0xA2, 0x13, 0xEE}};
        const std::optional<std::array<std::uint8_t, 6>> none;

        check(!acceptReply(false, radioA, none).accept,
              "a reply that is not a Hermes-Lite 2 is never believed");
        check(!acceptReply(false, radioA, radioA).accept,
              "not even from the radio we already latched");

        // THE LATCH, and what it does NOT buy. The first answer from the named
        // address is accepted whoever sent it -- an aim names an IP and the MAC
        // cannot be known before something replies, so there is nothing to
        // check it against. Saying otherwise is what the old comment did.
        const auto first = acceptReply(true, radioA, none);
        check(first.accept, "the first HL2 answer at a target IS believed");
        check(first.latch.has_value() && *first.latch == radioA,
              "and it latches the MAC that answered");

        // What it DOES buy: the responder cannot change underneath a live aim.
        // A DHCP reassignment, a second radio on the same address, or a NAT
        // answering for whatever is behind it today all produce a reading that
        // is continuous and wrong, which is the failure that survives longest
        // without being noticed.
        const auto changed = acceptReply(true, radioB, first.latch);
        check(!changed.accept,
              "a DIFFERENT MAC at the same address is refused once latched");
        check(!changed.latch.has_value(),
              "and a refused reply does not move the latch");

        const auto again = acceptReply(true, radioA, first.latch);
        check(again.accept, "while the latched radio keeps being believed");
        check(!again.latch.has_value(),
              "and an accepted reply from the latched radio does not re-latch");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_telemetry_cadence_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
