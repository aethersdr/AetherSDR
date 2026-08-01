// Unit test for QsoRecordStartPolicy (#4629).
//
// Client-Side recording depends on the `remote_audio_rx` stream, which exists
// only while PC Audio is enabled. Starting anyway produced a correctly named,
// header-only 44-byte WAV and no error at all.
//
// The case that matters most here is the one that must NOT change:
// RADIO-SIDE RECORDING DOES NOT DEPEND ON PC AUDIO. It is a `slice set <n>
// record=1` command handled entirely by the radio, and it is the standing
// answer for an operator who monitors on the radio's own speaker. If the guard
// ever starts blocking it, that operator loses the one path that still worked.
//
// Pure policy, so no Qt, no event loop, no settings store — and constexpr, so
// the whole table is also checked at compile time.

#include "core/QsoRecordStartPolicy.h"

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_DECISION(clientSide, pcAudio, seamNative, expected) do { \
    const RecordStartDecision got_ = \
        evaluateRecordStart((clientSide), (pcAudio), (seamNative)); \
    if (got_ != (expected)) { \
        std::fprintf(stderr, "FAIL %s:%d  evaluateRecordStart(clientSide=%d, " \
                             "pcAudio=%d, seamNative=%d) = %d, expected %d\n", \
                     __FILE__, __LINE__, int(clientSide), int(pcAudio), \
                     int(seamNative), int(got_), int(expected)); \
        ++g_failures; \
    } \
} while (0)

// Compile-time proof of the whole truth table. A regression that makes any of
// these false is a build error, not just a test failure.
//                                     client  pcAudio  seamNative
static_assert(evaluateRecordStart(true,  false, false) == RecordStartDecision::BlockedPcAudioDisabled,
              "Flex client-side without PC Audio must be refused");
static_assert(evaluateRecordStart(true,  true,  false) == RecordStartDecision::Allow,
              "Flex client-side with PC Audio must be allowed");
static_assert(evaluateRecordStart(false, false, false) == RecordStartDecision::Allow,
              "RADIO-SIDE MUST NEVER BE BLOCKED: it does not use PC Audio");
static_assert(evaluateRecordStart(false, true,  false) == RecordStartDecision::Allow,
              "radio-side is allowed regardless of PC Audio");
static_assert(evaluateRecordStart(true,  false, true)  == RecordStartDecision::Allow,
              "SEAM-NATIVE AUDIO (sim/HL2) MUST NEVER BE BLOCKED: it bypasses "
              "remote_audio_rx, so PC Audio is not in its path");
static_assert(evaluateRecordStart(true,  true,  true)  == RecordStartDecision::Allow,
              "seam-native with PC Audio on is allowed");

int main()
{
    // ── The bug: Flex, client-side, PC Audio off ────────────────────────────
    // The only combination that may be refused.
    EXPECT_DECISION(true, false, false, RecordStartDecision::BlockedPcAudioDisabled);

    // ── Client-side with PC Audio on: the working configuration ─────────────
    EXPECT_DECISION(true, true, false, RecordStartDecision::Allow);

    // ── Radio-side: PC Audio is irrelevant, both ways ───────────────────────
    // This is the operator's escape hatch when they listen on the radio rather
    // than the PC. Blocking it would be a worse bug than the one being fixed.
    EXPECT_DECISION(false, false, false, RecordStartDecision::Allow);
    EXPECT_DECISION(false, true,  false, RecordStartDecision::Allow);

    // ── Seam-native backends: SimBackend is the live counter-example ────────
    // Sim owns its RX audio (ownsRxAudio()==true) but neither host-modulates
    // nor transmits, so MainWindow does NOT lock PC Audio on for it. An
    // operator in demo mode can turn PC Audio off and the sim's audio still
    // reaches the recorder over the seam. Blocking that would refuse a
    // recording that works. HL2 lands here too (it does lock PC Audio on, so
    // the pcAudio=false row is unreachable there — but it costs nothing to be
    // right for both).
    EXPECT_DECISION(true, false, true, RecordStartDecision::Allow);
    EXPECT_DECISION(true, true,  true, RecordStartDecision::Allow);

    if (g_failures == 0)
        std::fprintf(stderr, "qso_record_start_policy_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
