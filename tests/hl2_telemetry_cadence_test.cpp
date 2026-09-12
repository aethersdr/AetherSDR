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

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_telemetry_cadence_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
