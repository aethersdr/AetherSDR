// Unit test for QsoRecordStartPolicy (#4629).
//
// Client-Side recording depends on the `remote_audio_rx` stream, which exists
// only while PC Audio is enabled. Starting anyway produced a correctly named,
// header-only 44-byte WAV and no error at all.
//
// RADIO-SIDE RECORDING MUST NEVER BE BLOCKED — it is the standing answer for an
// operator who monitors on the radio's own speaker, and it does not depend on
// PC Audio. That guarantee is enforced by the ROUTING layer, not by this
// policy: MainWindow_Wiring and the MIDI binding test RecordingMode and send
// radio-side straight to SliceModel::setRecordOn(), so a radio-side recording
// never consults this file at all.
//
// What this policy answers is narrower — "may THE CLIENT RECORDER start?" — and
// in Radio-Side mode the answer is no, because QsoRecorder only ever writes a
// local file. An earlier revision returned Allow here on the theory that
// radio-side must not be blocked; that conflated the two questions and let a
// non-routing caller (AutomationServer::doRecord) open a local WAV in Radio-Side
// mode, leaving exactly the spurious header-only file this change exists to
// prevent.
//
// Pure policy, so no Qt, no event loop, no settings store — and constexpr, so
// the whole table is also checked at compile time.

#include "core/QsoRecordStartPolicy.h"

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_TRUE_MSG(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (msg)); \
        ++g_failures; \
    } \
} while (0)

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
// Radio-Side: the CLIENT recorder must not run, and the reason must be its own
// — never BlockedPcAudioDisabled, which would explain the refusal with a cause
// that has nothing to do with it.
static_assert(evaluateRecordStart(false, false, false) == RecordStartDecision::BlockedRecordingModeIsRadio,
              "Radio-Side must not start the client recorder");
static_assert(evaluateRecordStart(false, true,  false) == RecordStartDecision::BlockedRecordingModeIsRadio,
              "Radio-Side is refused regardless of PC Audio, and for its own reason");
static_assert(evaluateRecordStart(false, false, true)  == RecordStartDecision::BlockedRecordingModeIsRadio,
              "Radio-Side outranks the seam-native exemption: the radio is recording");
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

    // ── Radio-side: the CLIENT recorder is the wrong tool, whatever the audio ─
    // The operator's radio-side recording still happens — it is routed to
    // SliceModel before this policy is ever consulted (see the header comment).
    // What is refused is QsoRecorder writing a local file it cannot fill.
    EXPECT_DECISION(false, false, false, RecordStartDecision::BlockedRecordingModeIsRadio);
    EXPECT_DECISION(false, true,  false, RecordStartDecision::BlockedRecordingModeIsRadio);
    EXPECT_DECISION(false, false, true,  RecordStartDecision::BlockedRecordingModeIsRadio);

    // The reason must never be reported as a PC Audio problem — an operator in
    // Radio-Side mode told to go enable PC Audio has been sent the wrong way.
    EXPECT_TRUE_MSG(evaluateRecordStart(false, false, false)
                        != RecordStartDecision::BlockedPcAudioDisabled,
                    "Radio-Side must not be explained as a PC Audio failure");

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
