// SplitAudioProfile parsing/serialisation rules (#2242).
//
// The interesting behaviour here is not the round-trip, it is the three ways a
// stored profile can be wrong — absent, wrong-typed, wrong-versioned — and the
// distinction between "never learned" and "learned a default-looking value",
// which is what stops the feature moving slice audio nobody asked it to move.

#include "core/SplitAudioProfile.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <cstdlib>

using AetherSDR::SplitAudioProfile;

namespace {

int g_failures = 0;

void check(bool cond, const char* what)
{
    if (cond) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

QJsonObject parseObj(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

// ── A profile nobody has taught anything ────────────────────────────────────
void testEmptyIsTodaysBehaviour()
{
    const SplitAudioProfile p;
    check(!p.hasLearnedState(), "default profile has learned nothing");
    check(p.monitor == SplitAudioProfile::Monitor::Solo,
          "monitor defaults to solo (XFC-faithful)");

    const SplitAudioProfile fromEmpty = SplitAudioProfile::fromJson(QJsonObject{});
    check(!fromEmpty.hasLearnedState(), "empty object learns nothing");
    check(fromEmpty.monitor == SplitAudioProfile::Monitor::Solo,
          "empty object keeps the solo default");
}

// ── has* is the whole point: 50 chosen != 50 defaulted ──────────────────────
void testTouchedIsDistinctFromDefaultValue()
{
    const auto p = SplitAudioProfile::fromJson(parseObj(R"({"v":1,"txPan":50})"));
    check(p.hasTxPan, "an explicitly stored centre pan counts as learned");
    check(p.txPan == 50, "centre pan value survives");
    check(!p.hasTxGain, "an absent gain is NOT learned");
    check(!p.hasRxPan,  "an absent rx pan is NOT learned");
    check(p.hasLearnedState(), "one learned field is enough to have state");

    // ...and the distinction has to survive a round-trip, or it only holds
    // until the next restart.
    const auto again = SplitAudioProfile::fromJson(p.toJson());
    check(again.hasTxPan && again.txPan == 50, "learned centre pan round-trips");
    check(!again.hasTxGain, "un-learned gain stays un-learned across a round-trip");
    check(!p.toJson().contains(QStringLiteral("txGain")),
          "an un-learned field is omitted, not written as a default");
}

void testFullRoundTrip()
{
    SplitAudioProfile p;
    p.hasTxMute = true; p.txMuted = false;
    p.hasTxGain = true; p.txGain  = 40;
    p.hasTxPan  = true; p.txPan   = 100;
    p.hasRxPan  = true; p.rxPan   = 0;
    p.monitor = SplitAudioProfile::Monitor::Both;

    const auto r = SplitAudioProfile::fromJson(p.toJson());
    check(r.hasTxMute && !r.txMuted, "tx mute round-trips");
    check(r.hasTxGain && r.txGain == 40, "tx gain round-trips");
    check(r.hasTxPan  && r.txPan  == 100, "tx pan round-trips");
    check(r.hasRxPan  && r.rxPan  == 0,   "rx pan round-trips");
    check(r.monitor == SplitAudioProfile::Monitor::Both, "monitor round-trips");
}

// ── The three ways a stored object can be wrong ─────────────────────────────
void testVersionGate()
{
    const auto future = SplitAudioProfile::fromJson(
        parseObj(R"({"v":2,"txPan":100,"txMuted":false})"));
    check(!future.hasLearnedState(),
          "a future version is not partially mined for same-named fields");

    const auto noVer = SplitAudioProfile::fromJson(parseObj(R"({"txPan":100})"));
    check(!noVer.hasLearnedState(), "a versionless object is not trusted");

    const auto strVer = SplitAudioProfile::fromJson(parseObj(R"({"v":"1","txPan":100})"));
    check(!strVer.hasLearnedState(), "a non-numeric version is not trusted");
}

void testWrongTypeDropsLearnedStateButKeepsMonitor()
{
    const auto p = SplitAudioProfile::fromJson(
        parseObj(R"({"v":1,"monitor":"both","txPan":"hard right","txGain":40})"));
    check(!p.hasLearnedState(),
          "one wrong-typed field invalidates the whole learned arrangement");
    check(p.monitor == SplitAudioProfile::Monitor::Both,
          "a deliberate monitor choice survives a rotted learned value");

    const auto m = SplitAudioProfile::fromJson(
        parseObj(R"({"v":1,"monitor":7,"txPan":100})"));
    check(m.monitor == SplitAudioProfile::Monitor::Solo,
          "a wrong-typed monitor falls back to solo");
    check(m.hasTxPan && m.txPan == 100,
          "a wrong-typed monitor does not invalidate the learned values");
}

void testClamping()
{
    const auto p = SplitAudioProfile::fromJson(
        parseObj(R"({"v":1,"txPan":150,"txGain":-20,"rxPan":100})"));
    check(p.hasTxPan  && p.txPan  == 100, "over-range pan clamps to 100");
    check(p.hasTxGain && p.txGain == 0,   "under-range gain clamps to 0");
    check(p.hasRxPan  && p.rxPan  == 100, "an in-range boundary is untouched");
}

// ── Forget clears what was inferred, not what was chosen ────────────────────
void testForgetKeepsMonitorPreference()
{
    SplitAudioProfile p;
    p.hasTxMute = true; p.txMuted = false;
    p.hasRxPan  = true; p.rxPan   = 0;
    p.monitor = SplitAudioProfile::Monitor::Both;

    p.forgetLearnedState();
    check(!p.hasLearnedState(), "forget drops every learned value");
    check(p.monitor == SplitAudioProfile::Monitor::Both,
          "forget keeps the chosen monitor mode");
    check(!p.toJson().contains(QStringLiteral("txMuted")),
          "a forgotten profile writes no learned keys");
    check(SplitAudioProfile::fromJson(p.toJson()).monitor
              == SplitAudioProfile::Monitor::Both,
          "the kept monitor mode survives storage");
}

// ── SplitAudioRecorder: one split's worth of operator changes ───────────────

using AetherSDR::SplitAudioRecorder;

// A split the operator never touches must store nothing, or the next split
// starts differently from this one for no reason the operator can point at.
void testUntouchedSplitLearnsNothing()
{
    SplitAudioRecorder r;
    r.arm(/*rxPanBefore=*/50, /*txMuted=*/true, /*txGain=*/50, /*txPan=*/50);
    check(r.armed(), "arm() arms");

    const auto p = r.merge(SplitAudioProfile{});
    check(!p.hasLearnedState(),
          "the seed values are NOT learned — only note*() calls are");
    check(r.rxPanToRestore() < 0, "an untouched RX pan is not restored");
}

// The whole #2242 worked example, end to end.
void testWorkedExample()
{
    SplitAudioRecorder r;
    r.arm(/*rxPanBefore=*/50, /*txMuted=*/true, /*txGain=*/50, /*txPan=*/50);
    r.noteTxMute(false);   // unmute the TX slice to hear the pileup
    r.noteTxPan(100);      // TX hard right
    r.noteTxGain(40);      // and quieter than the DX
    r.noteRxPan(0);        // DX hard left

    const auto p = r.merge(SplitAudioProfile{});
    check(p.hasTxMute && !p.txMuted, "unmute is learned");
    check(p.hasTxPan  && p.txPan  == 100, "TX pan is learned");
    check(p.hasTxGain && p.txGain == 40,  "TX gain is learned");
    check(p.hasRxPan  && p.rxPan  == 0,   "RX pan is learned");
    check(r.rxPanToRestore() == 50,
          "the RX slice goes back to the pan it had before the split");
}

// #2242: "Muting the TX slice during a split makes the next split start muted,
// so the operator can return to today's behavior by muting it once."
void testMutingOnceReturnsToTheOldBehaviour()
{
    SplitAudioProfile stored;
    stored.hasTxMute = true; stored.txMuted = false;
    stored.hasTxPan  = true; stored.txPan   = 100;

    SplitAudioRecorder r;
    r.arm(50, /*txMuted=*/false, 40, 100);
    r.noteTxMute(true);    // the operator mutes it and changes nothing else

    const auto p = r.merge(stored);
    check(p.hasTxMute && p.txMuted, "the mute is learned");
    check(!p.hasTxPan,
          "a pan the operator did not set this split stops coming back");
}

// Replace, don't accumulate — otherwise an arrangement can only ever grow.
void testMergeReplacesLearnedStateWholesale()
{
    SplitAudioProfile stored;
    stored.hasTxGain = true; stored.txGain = 40;
    stored.hasRxPan  = true; stored.rxPan  = 0;
    stored.monitor = SplitAudioProfile::Monitor::Both;

    SplitAudioRecorder r;
    r.arm(50, true, 40, 50);
    r.noteTxPan(100);      // a different field entirely

    const auto p = r.merge(stored);
    check(p.hasTxPan && p.txPan == 100, "this split's value is stored");
    check(!p.hasTxGain, "last split's gain does not survive");
    check(!p.hasRxPan,  "last split's rx pan does not survive");
    check(p.monitor == SplitAudioProfile::Monitor::Both,
          "the chosen monitor mode is not learned state and does survive");
}

// The recorder outlives the TX slice on purpose: onSliceRemoved runs with the
// model object already destroyed, so nothing can be read off it there.
void testRecorderSurvivesTheSliceItDescribes()
{
    SplitAudioRecorder r;
    r.arm(0, true, 50, 50);
    r.noteTxMute(false);
    r.noteTxGain(30);
    // ...the TX slice is now gone. No further note*() calls are possible.
    const auto p = r.merge(SplitAudioProfile{});
    check(p.hasTxMute && !p.txMuted && p.hasTxGain && p.txGain == 30,
          "everything needed to store the arrangement is already held here");
}

void testDisarmedRecorderIgnoresNotes()
{
    SplitAudioRecorder r;
    r.noteTxPan(100);      // never armed — e.g. an externally started split
    check(!r.armed(), "an unarmed recorder stays unarmed");
    check(!r.merge(SplitAudioProfile{}).hasLearnedState(),
          "notes to an unarmed recorder are dropped");

    r.arm(50, true, 50, 50);
    r.noteTxPan(100);
    r.disarm();
    r.noteTxGain(10);
    check(!r.merge(SplitAudioProfile{}).hasLearnedState(),
          "disarm() drops what was learned and stops listening");
}

void testRecorderClamps()
{
    SplitAudioRecorder r;
    r.arm(50, true, 50, 50);
    r.noteTxGain(500);
    r.noteTxPan(-5);
    const auto p = r.merge(SplitAudioProfile{});
    check(p.txGain == 100, "an out-of-range gain clamps on the way out");
    check(p.txPan  == 0,   "an out-of-range pan clamps on the way out");
}

// A split that began with no RX slice (rxPanBefore == -1) must not "restore"
// the RX pan to a sentinel.
void testNoRxSliceMeansNoRestore()
{
    SplitAudioRecorder r;
    r.arm(/*rxPanBefore=*/-1, true, 50, 50);
    r.noteRxPan(0);
    check(r.rxPanToRestore() < 0, "an unknown pre-split pan is never restored");
}

}  // namespace

int main()
{
    testEmptyIsTodaysBehaviour();
    testTouchedIsDistinctFromDefaultValue();
    testFullRoundTrip();
    testVersionGate();
    testWrongTypeDropsLearnedStateButKeepsMonitor();
    testClamping();
    testForgetKeepsMonitorPreference();
    testUntouchedSplitLearnsNothing();
    testWorkedExample();
    testMutingOnceReturnsToTheOldBehaviour();
    testMergeReplacesLearnedStateWholesale();
    testRecorderSurvivesTheSliceItDescribes();
    testDisarmedRecorderIgnoresNotes();
    testRecorderClamps();
    testNoRxSliceMeansNoRestore();

    if (g_failures) {
        std::fprintf(stderr, "split_audio_profile_test: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("split_audio_profile_test: all checks passed\n");
    return EXIT_SUCCESS;
}
