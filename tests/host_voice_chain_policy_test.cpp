// When the Flex-shaped voice controls may write into the SHARED client DSP.
//
// PROC and the 8-band graphic EQ drive the same ClientComp/ClientEq the Aetherial
// strip edits (#4609). The strip's state PERSISTS; the models behind the
// Flex-shaped controls do not. So a connection edge that pushes the models
// unconditionally writes construction defaults over the operator's saved audio
// chain, and a family-swap unwind that fires unconditionally switches off a chain
// this code never touched.
//
// Both failures were live in review and both are one term of the predicates in
// core/HostVoiceChainPolicy.h, which is what this suite exercises directly —
// MainWindow evaluates the same functions, and the GUI link is not a test target
// (same limitation tests/radio_capability_gating_test.cpp documents).

#include "core/HostVoiceChainPolicy.h"

#include <cstdio>

using AetherSDR::hostVoiceChainRepushAllowed;
using AetherSDR::hostVoiceChainUnwindRequired;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

// Named for readability at the call sites below.
constexpr bool kConnected = true;
constexpr bool kDisconnected = false;
constexpr bool kHostModulates = true;
constexpr bool kRadioModulates = false;
constexpr bool kFlexPlane = true;
constexpr bool kNoFlexPlane = false;
constexpr bool kOwned = true;
constexpr bool kNotOwned = false;

}  // namespace

int main()
{
    // ── The re-push ────────────────────────────────────────────────────────
    //
    // THE REGRESSION THIS PINS: without the ownership term every HL2 connect
    // pushed EqualizerModel's construction default — eight bands at 0 dB with
    // both enables false — over the operator's persisted ClientEq slots 0..7,
    // and switched the strip's EQ and compressor off, for someone who had never
    // opened either applet. Drop `chainOwned` from the predicate and this fails.
    check(!hostVoiceChainRepushAllowed(kConnected, kHostModulates, kNotOwned),
          "a connect does NOT push voice-chain defaults the operator never set");
    check(hostVoiceChainRepushAllowed(kConnected, kHostModulates, kOwned),
          "a connect DOES re-push what the operator actually moved");

    // The other two terms still have to hold on their own.
    check(!hostVoiceChainRepushAllowed(kDisconnected, kHostModulates, kOwned),
          "a disconnect never re-pushes");
    check(!hostVoiceChainRepushAllowed(kConnected, kRadioModulates, kOwned),
          "a radio that modulates its own audio is never pushed to (no double EQ)");

    // ── The unwind ─────────────────────────────────────────────────────────
    //
    // THE REGRESSION THIS PINS: hostModulates is false for a Flex, so a plain
    // Flex connect falls through to the unwind branch — as does every Flex
    // disconnect. Unbounded, that disabled the operator's own Aetherial RX EQ,
    // TX EQ and TX compressor on a session that never went near an HL2. Those
    // tiles are family-agnostic by design (radio-capabilities-map.md): on a radio
    // with no hardware DSP they are the only audio DSP there is, and a Flex's
    // receive audio runs through ClientEq like every other family's.
    check(!hostVoiceChainUnwindRequired(kConnected, kRadioModulates, kFlexPlane,
                                        kNotOwned),
          "a plain Flex connect leaves the operator's Aetherial chain alone");
    check(!hostVoiceChainUnwindRequired(kDisconnected, kRadioModulates, kFlexPlane,
                                        kNotOwned),
          "a plain Flex disconnect leaves the operator's Aetherial chain alone");

    // The case the unwind exists for: graphic EQ moved on an HL2, then a Flex
    // attaches in the same process. Every apply now returns early on the family
    // check, but ClientEq still holds our eight peaking filters and is still
    // enabled — the radio's hardware EQ and ours would both apply.
    check(hostVoiceChainUnwindRequired(kConnected, kRadioModulates, kFlexPlane,
                                       kOwned),
          "a Flex attaching after an HL2 session takes our layout out of circuit");

    // Not on a Flex: nothing double-applies, so nothing is disabled. This is the
    // ordinary HL2 disconnect, where the operator's bands must survive for the
    // reconnect to restore them.
    check(!hostVoiceChainUnwindRequired(kDisconnected, kRadioModulates,
                                        kNoFlexPlane, kOwned),
          "an HL2 disconnect leaves the operator's bands in place for reconnect");

    // Still on the host-modulating backend: the mapping is exactly where it
    // belongs, whatever else the edge says.
    check(!hostVoiceChainUnwindRequired(kConnected, kHostModulates, kFlexPlane,
                                        kOwned),
          "a host-modulating backend is never unwound out from under itself");

    // The two are mutually exclusive on every input — one edge must never both
    // re-push the operator's layout and take it out of circuit.
    for (int bits = 0; bits < 16; ++bits) {
        const bool connected = bits & 1;
        const bool hostModulates = bits & 2;
        const bool flexPlane = bits & 4;
        const bool owned = bits & 8;
        if (hostVoiceChainRepushAllowed(connected, hostModulates, owned)
            && hostVoiceChainUnwindRequired(connected, hostModulates, flexPlane,
                                            owned)) {
            std::printf("[FAIL] push and unwind both true for bits %d\n", bits);
            ++g_failures;
        }
    }
    check(true, "no input both re-pushes and unwinds");

    if (g_failures == 0)
        std::printf("host_voice_chain_policy_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
