// SplitAudioProfile parsing/serialisation rules, the recorder's carry-forward
// and RX-pan restore rules, and the apply / Monitor TX hold sequences driven
// against production SliceModel objects (#2242).
//
// The interesting behaviour here is not the round-trip, it is the three ways a
// stored profile can be wrong — absent, wrong-typed, wrong-versioned — and the
// distinction between "never learned" and "learned a default-looking value",
// which is what stops the feature moving slice audio nobody asked it to move.

#include "gui/SplitAudioProfile.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QObject>

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

    const auto fracVer = SplitAudioProfile::fromJson(parseObj(R"({"v":1.5,"txPan":100})"));
    check(!fracVer.hasLearnedState(), "a fractional version is not version 1");

    const auto hugeVer = SplitAudioProfile::fromJson(parseObj(R"({"v":1e100,"txPan":100})"));
    check(!hugeVer.hasLearnedState(), "a huge version is rejected, not narrowed");
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

    // Beyond int's range. Narrowing before clamping is undefined behaviour, so
    // these clamp in the double domain.
    const auto big = SplitAudioProfile::fromJson(
        parseObj(R"({"v":1,"txPan":1e100,"txGain":-1e100})"));
    check(big.hasTxPan  && big.txPan  == 100, "a huge pan clamps to 100");
    check(big.hasTxGain && big.txGain == 0,   "a hugely negative gain clamps to 0");
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
using AetherSDR::SplitAudioApplyResult;
using AetherSDR::SliceModel;
using AetherSDR::SplitAudioOperatorEdit;
using SplitMonitorHold = AetherSDR::SplitMonitorHold<SliceModel>;

SplitAudioProfile workedExampleProfile()
{
    SplitAudioProfile p;
    p.hasTxMute = true; p.txMuted = false;
    p.hasTxGain = true; p.txGain  = 40;
    p.hasTxPan  = true; p.txPan   = 100;
    p.hasRxPan  = true; p.rxPan   = 0;
    return p;
}

// A first split the operator never touches must store nothing, or the next
// split starts differently from this one for no reason the operator can see.
void testUntouchedSplitLearnsNothing()
{
    SplitAudioRecorder r;
    r.arm(/*rxPanBefore=*/50, /*rxPanMovedByApply=*/false);
    check(r.armed(), "arm() arms");

    const auto p = r.merge(SplitAudioProfile{});
    check(!p.hasLearnedState(), "an untouched first split learns nothing");
    check(r.rxPanToRestore() < 0, "an unmoved RX pan is not restored");
}

// The whole #2242 worked example, end to end.
void testWorkedExample()
{
    SplitAudioOperatorEdit op;   // these are the operator's own edits
    SplitAudioRecorder r;
    r.arm(/*rxPanBefore=*/50, false);
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

// Teach once, then split, split, split without touching anything: the
// arrangement must still be there on the third. (Review blocker 1: a
// wholesale-replace merge emptied it on the second untouched exit.)
void testArrangementSurvivesUntouchedReplays()
{
    SplitAudioProfile stored = workedExampleProfile();
    for (int split = 2; split <= 4; ++split) {
        SplitAudioRecorder r;
        r.arm(/*rxPanBefore=*/50, /*rxPanMovedByApply=*/true);
        stored = r.merge(stored);          // ended with no edits at all
        check(stored.hasTxMute && !stored.txMuted
                  && stored.hasTxGain && stored.txGain == 40
                  && stored.hasTxPan  && stored.txPan  == 100
                  && stored.hasRxPan  && stored.rxPan  == 0,
              "an untouched replayed split keeps the whole arrangement");
        check(r.rxPanToRestore() == 50,
              "a replayed RX pan is put back even though nobody touched it");
    }
}

// Only the field the operator touched is replaced; the rest carries forward.
void testTouchedFieldReplacesOnlyItself()
{
    SplitAudioOperatorEdit op;   // these are the operator's own edits
    SplitAudioProfile stored = workedExampleProfile();
    stored.monitor = SplitAudioProfile::Monitor::Both;

    SplitAudioRecorder r;
    r.arm(50, true);
    r.noteTxGain(25);

    const auto p = r.merge(stored);
    check(p.hasTxGain && p.txGain == 25, "the touched gain is replaced");
    check(p.hasTxPan && p.txPan == 100 && p.hasRxPan && p.rxPan == 0
              && p.hasTxMute && !p.txMuted,
          "the untouched fields carry forward");
    check(p.monitor == SplitAudioProfile::Monitor::Both,
          "the chosen monitor mode is kept");
}

// #2242: "Muting the TX slice during a split makes the next split start muted,
// so the operator can return to today's behavior by muting it once."
void testMutingOnceReturnsToTheOldBehaviour()
{
    SplitAudioOperatorEdit op;   // these are the operator's own edits
    SplitAudioProfile stored = workedExampleProfile();
    stored.monitor = SplitAudioProfile::Monitor::Both;

    SplitAudioRecorder r;
    r.arm(50, true);
    r.noteTxMute(true);    // the operator mutes it and changes nothing else

    const auto p = r.merge(stored);
    check(!p.hasLearnedState(),
          "ending a split muted clears the arrangement: back to pre-#2242");
    check(p.monitor == SplitAudioProfile::Monitor::Both,
          "...but not the chosen monitor mode");
    check(r.rxPanToRestore() == 50, "and the RX pan still goes back");

    // Muting and then unmuting again within the split is not a reset.
    SplitAudioRecorder r2;
    r2.arm(50, true);
    r2.noteTxMute(true);
    r2.noteTxMute(false);
    check(r2.merge(workedExampleProfile()).hasTxPan,
          "the mute that counts is the one the split ended with");
}

// Review blocker 2: the pre-split RX pan must be the one from BEFORE the
// replay moved it, including when the operator pans it again during a later
// split.
void testRxPanRestoresToTheTruePreSplitValue()
{
    SplitAudioOperatorEdit op;   // these are the operator's own edits
    SplitAudioRecorder r;
    r.arm(/*rxPanBefore=*/50, /*rxPanMovedByApply=*/true);  // replay moved it to 0
    r.noteRxPan(20);                                        // operator nudges it
    check(r.rxPanToRestore() == 50,
          "a manual edit in a replayed split still restores the pre-split pan");
    check(r.merge(workedExampleProfile()).rxPan == 20,
          "and the nudge is what is remembered");
}

// The recorder outlives the TX slice on purpose: onSliceRemoved runs with the
// model object already destroyed, so nothing can be read off it there.
void testRecorderSurvivesTheSliceItDescribes()
{
    SplitAudioOperatorEdit op;   // these are the operator's own edits
    SplitAudioRecorder r;
    r.arm(0, false);
    r.noteTxMute(false);
    r.noteTxGain(30);
    // ...the TX slice is now gone. No further note*() calls are possible.
    const auto p = r.merge(SplitAudioProfile{});
    check(p.hasTxMute && !p.txMuted && p.hasTxGain && p.txGain == 30,
          "everything needed to store the arrangement is already held here");
}

void testDisarmedRecorderIgnoresNotes()
{
    SplitAudioOperatorEdit op;   // these are the operator's own edits
    SplitAudioRecorder r;
    r.noteTxPan(100);      // never armed — e.g. an externally started split
    check(!r.armed(), "an unarmed recorder stays unarmed");
    check(!r.merge(SplitAudioProfile{}).hasLearnedState(),
          "notes to an unarmed recorder are dropped");

    r.arm(50, false);
    r.noteTxPan(100);
    r.disarm();
    r.noteTxGain(10);
    check(!r.merge(SplitAudioProfile{}).hasLearnedState(),
          "disarm() drops what was learned and stops listening");
}

void testRecorderClamps()
{
    SplitAudioOperatorEdit op;   // these are the operator's own edits
    SplitAudioRecorder r;
    r.arm(50, false);
    r.noteTxMute(false);
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
    SplitAudioOperatorEdit op;   // these are the operator's own edits
    SplitAudioRecorder r;
    r.arm(/*rxPanBefore=*/-1, true);
    r.noteRxPan(0);
    check(r.rxPanToRestore() < 0, "an unknown pre-split pan is never restored");
}

// ── The production sequence against real SliceModels ───────────────────────
//
// What MainWindow does, in its order: apply, arm with what the apply found,
// feed the *CommandIssued signals into the recorder, merge and restore on
// exit. The simulator cannot create a second slice, so this is where the
// enter/exit/enter sequence runs end to end.

struct SplitRun {
    SplitAudioRecorder rec;
    SplitAudioApplyResult applied;
    QList<QMetaObject::Connection> conns;
    bool applying{false};

    void enter(const SplitAudioProfile& stored, SliceModel& rx, SliceModel& tx)
    {
        applying = true;
        applied = AetherSDR::applySplitAudioProfile(stored, &rx, &tx);
        applying = false;
        rec.arm(applied.rxPanBefore, applied.rxPanMoved);
        conns << QObject::connect(&tx, &SliceModel::audioMuteCommandIssued,
                                  [this](bool m) { if (!applying) rec.noteTxMute(m); });
        conns << QObject::connect(&tx, &SliceModel::audioGainCommandIssued,
                                  [this](int g) { if (!applying) rec.noteTxGain(g); });
        conns << QObject::connect(&tx, &SliceModel::audioPanCommandIssued,
                                  [this](int p) { if (!applying) rec.noteTxPan(p); });
        conns << QObject::connect(&rx, &SliceModel::audioPanCommandIssued,
                                  [this](int p) { if (!applying) rec.noteRxPan(p); });
    }

    SplitAudioProfile exit(const SplitAudioProfile& stored, SliceModel& rx)
    {
        const auto next = rec.merge(stored);
        if (const int restore = rec.rxPanToRestore(); restore >= 0) {
            applying = true;
            rx.setAudioPan(restore);
            applying = false;
        }
        for (const auto& c : conns) QObject::disconnect(c);
        return next;
    }
};

void testRepeatedSplitsOnRealSlices()
{
    SliceModel rx(0);
    rx.setAudioPan(50);
    SplitAudioProfile stored;

    // Split 1: nothing remembered, so the TX slice is born muted. Teach it.
    {
        SliceModel tx(1);
        SplitRun run;
        run.enter(stored, rx, tx);
        check(tx.flexAudioMute(), "split 1: a new TX slice is muted");
        check(!run.applied.restored, "split 1: nothing to restore");
        {
            SplitAudioOperatorEdit op;
            tx.setAudioMute(false);
            tx.setAudioGain(40.0f);
            tx.setAudioPan(100);
            rx.setAudioPan(0);
        }
        stored = run.exit(stored, rx);
        check(rx.flexAudioPan() == 50, "split 1: RX pan goes back to 50");
    }

    // Splits 2 and 3: nobody touches anything. Each must come back complete
    // and each exit must put the RX pan back.
    for (int split = 2; split <= 3; ++split) {
        SliceModel tx(split);
        SplitRun run;
        run.enter(stored, rx, tx);
        check(!tx.flexAudioMute() && tx.flexAudioPan() == 100
                  && static_cast<int>(tx.flexAudioGain()) == 40,
              "untouched replays: the TX arrangement comes back");
        check(rx.flexAudioPan() == 0, "untouched replays: RX is panned left");
        check(run.applied.rxPanBefore == 50,
              "the pre-split RX pan is read BEFORE the replay moves it");
        stored = run.exit(stored, rx);
        check(rx.flexAudioPan() == 50, "untouched replays: RX pan is restored");
        check(stored.hasLearnedState() && stored.rxPan == 0,
              "untouched replays: nothing is forgotten");
    }

    // Split 4: the operator nudges the RX pan. Restore still goes to 50.
    {
        SliceModel tx(4);
        SplitRun run;
        run.enter(stored, rx, tx);
        { SplitAudioOperatorEdit op; rx.setAudioPan(20); }
        stored = run.exit(stored, rx);
        check(rx.flexAudioPan() == 50, "a nudged RX pan restores to pre-split");
        check(stored.rxPan == 20, "and the nudge is remembered");
    }

    // Split 5: mute the TX slice. Next split is pre-#2242 again.
    {
        SliceModel tx(5);
        SplitRun run;
        run.enter(stored, rx, tx);
        { SplitAudioOperatorEdit op; tx.setAudioMute(true); }
        stored = run.exit(stored, rx);
        check(!stored.hasLearnedState(), "muting once clears the arrangement");
        check(rx.flexAudioPan() == 50, "and RX pan is still restored");
    }
    {
        SliceModel tx(6);
        SplitRun run;
        run.enter(stored, rx, tx);
        check(tx.flexAudioMute() && tx.flexAudioPan() == 50
                  && rx.flexAudioPan() == 50,
              "after muting once, the next split is muted and moves nothing");
        run.exit(stored, rx);
    }
}

// Automated review: TCI rx_mute/rx_balance, SmartCAT ZZMB/ZZLF, rigctld MUTE,
// Mute All, RADE and memory recall all call the same SliceModel setters and so
// emit the same *CommandIssued signals. None of them is the operator's choice:
// a logger muting VFO B must not wipe the arrangement, and a CAT pan must not
// be replayed on every split.
void testWritesOutsideTheOperatorScopeAreNotLearned()
{
    SliceModel rx(0);
    rx.setAudioPan(50);
    SliceModel tx(1);
    SplitRun run;
    const SplitAudioProfile stored = workedExampleProfile();
    run.enter(stored, rx, tx);

    tx.setAudioMute(true);     // e.g. TCI rx_mute:1,true / SmartCAT ZZMB1
    tx.setAudioPan(0);         // e.g. TCI rx_balance / SmartCAT ZZLF
    rx.setAudioPan(70);
    const auto next = run.exit(stored, rx);
    check(next.hasTxMute && !next.txMuted && next.txPan == 100 && next.rxPan == 0,
          "remote/app-internal writes teach nothing and wipe nothing");
    check(rx.flexAudioPan() == 50,
          "the replayed RX pan is still restored to its pre-split value");
}

// "Forget remembered audio" mid-split (bot review, Codex review, rnash2 on a
// FLEX-6600): clearing the stored profile is not enough if the recorder still
// holds this split's edits, because the split's end merges them straight back.
void testForgetMidSplitDropsPendingEditsKeepsRestore()
{
    SplitAudioOperatorEdit op;
    SplitAudioRecorder r;
    r.arm(/*rxPanBefore=*/50, /*rxPanMovedByApply=*/false);
    check(!r.hasPendingLearning(), "a fresh split has nothing pending");
    r.noteTxPan(100);
    r.noteRxPan(0);
    check(r.hasPendingLearning(), "operator edits are pending until the split ends");

    SplitAudioProfile stored;               // Forget already cleared this
    stored.monitor = SplitAudioProfile::Monitor::Both;
    r.forgetTouched();
    check(!r.hasPendingLearning(), "forget drops the pending edits");
    const auto p = r.merge(stored);
    check(!p.hasLearnedState(), "the split's end writes nothing back after forget");
    check(p.monitor == SplitAudioProfile::Monitor::Both, "monitor mode is kept");
    check(r.rxPanToRestore() == 50,
          "the RX pan the operator moved is still put back at exit");
}

// Codex review + automated pass: a hold must restore into the SAME slice
// objects it changed. A quick reconnect reclaims those objects (RadioModel
// keeps them), so the release must still work; a new session's slices that
// merely reuse the ids must never be written.
void testHoldRestoresOnlyIntoTheSameObjects()
{
    SplitMonitorHold hold;
    SliceModel rx(0), tx(1);
    tx.setAudioMute(true);
    hold.begin(&rx, &tx, SplitAudioProfile::Monitor::Solo);

    SliceModel rx2(0), tx2(1);            // same ids, different objects
    rx2.setAudioMute(true);
    tx2.setAudioMute(false);
    SplitMonitorHold copy = hold;         // what a deferred release carries
    copy.end(&rx2, &tx2);
    check(rx2.flexAudioMute() && !tx2.flexAudioMute(),
          "same-id slices that are not the held objects are not written");
    check(rx.flexAudioMute() && !tx.flexAudioMute(),
          "...and the held objects keep the hold's state until released");

    hold.end(&rx, &tx);                   // reclaimed: the same objects
    check(!rx.flexAudioMute() && tx.flexAudioMute(),
          "the same objects (a reclaim after reconnect) are restored");
}

// A slice the hold changed was destroyed and a new one took its id: the
// guarded pointer is null, so nothing is written anywhere.
void testHoldOnDestroyedSlicesWritesNothing()
{
    SplitMonitorHold hold;
    {
        SliceModel rx(0), tx(1);
        tx.setAudioMute(true);
        hold.begin(&rx, &tx, SplitAudioProfile::Monitor::Solo);
    }
    SliceModel rx2(0), tx2(1);
    rx2.setAudioMute(false);
    tx2.setAudioMute(false);
    hold.end(&rx2, &tx2);
    check(!rx2.flexAudioMute() && !tx2.flexAudioMute(),
          "a destroyed slice's successors are untouched");
    check(!hold.active(), "and the hold is over");
}

// MainWindow parks a release whose slices are alive but out of the live map
// (a reconnect in progress) and completes it on reclaim. That decision rests
// on these accessors naming the held objects and going null once they die.
void testHoldExposesItsObjectsForTheReclaimCheck()
{
    SplitMonitorHold hold;
    auto* rx = new SliceModel(0);
    SliceModel tx(1);
    tx.setAudioMute(true);
    hold.begin(rx, &tx, SplitAudioProfile::Monitor::Solo);
    check(hold.rxObject() == rx && hold.txObject() == &tx,
          "the hold names the objects it changed");
    SplitMonitorHold pending = hold;      // a parked release keeps the copy
    delete rx;                            // e.g. pruned instead of reclaimed
    check(pending.rxObject() == nullptr && pending.txObject() == &tx,
          "a destroyed held slice reads as gone, not parked");
    pending.end(nullptr, &tx);
    check(tx.flexAudioMute(), "the surviving held slice is still restored");
}

// Codex review: KiwiSDR snapshots the Flex mute when it takes a slice over and
// restores that snapshot when it lets go. MainWindow ends any Monitor TX hold
// on the slice FIRST (endSplitMonitorForSlice in the Kiwi takeover), so the
// snapshot is the operator's real mute, not the hold's temporary one.
void testKiwiTakeoverAfterEndingTheHoldRestoresAudible()
{
    SliceModel rx(0), tx(1);
    tx.setAudioMute(true);
    SplitMonitorHold hold;
    hold.begin(&rx, &tx, SplitAudioProfile::Monitor::Solo);
    check(rx.flexAudioMute(), "setup: the hold muted RX");

    hold.end(&rx, &tx);                               // MainWindow's order
    const bool kiwiPreviousMute = rx.flexAudioMute(); // Kiwi's snapshot
    rx.setExternalReceiveAudioReplacementMute(true);
    rx.setExternalReceiveAudioReplacementMute(false, kiwiPreviousMute);
    check(!rx.flexAudioMute(),
          "after Kiwi lets go the receiver is audible again");
}

// ── Monitor TX hold against real SliceModels ────────────────────────────────

// Review blocker 3: an RX slice whose audio a KiwiSDR/DAX replacement owns is
// skipped on press — and must be skipped on release too, or setAudioMute()
// writes the REPLACEMENT mute and silences a receiver the hold never touched.
void testMonitorLeavesReplacedRxAlone()
{
    SliceModel rx(0), tx(1);
    tx.setAudioMute(true);
    rx.setExternalReceiveAudioReplacementMute(true);   // native mute forced on
    check(rx.externalReceiveReplacementActive() && !rx.audioMute()
              && rx.flexAudioMute(),
          "setup: replacement audible, native muted");

    SplitMonitorHold hold;
    check(hold.begin(&rx, &tx, SplitAudioProfile::Monitor::Solo),
          "a hold starts on a pair whose RX is replaced");
    check(!tx.flexAudioMute(), "during: TX is audible");
    check(!rx.audioMute(), "during: the replaced RX is not touched");
    hold.end(&rx, &tx);
    check(tx.flexAudioMute(), "after: TX is muted again");
    check(!rx.audioMute(), "after: the replaced RX is STILL audible");
    check(!hold.active(), "the hold is over");
}

void testMonitorSoloRestoresExactly()
{
    SliceModel rx(0), tx(1);
    tx.setAudioMute(true);
    SplitMonitorHold hold;
    hold.begin(&rx, &tx, SplitAudioProfile::Monitor::Solo);
    check(rx.flexAudioMute() && !tx.flexAudioMute(), "solo: RX muted, TX audible");
    hold.end(&rx, &tx);
    check(!rx.flexAudioMute() && tx.flexAudioMute(), "solo: both put back");

    // An already-unmuted TX (a learned arrangement) is not re-muted on release.
    tx.setAudioMute(false);
    hold.begin(&rx, &tx, SplitAudioProfile::Monitor::Solo);
    hold.end(&rx, &tx);
    check(!tx.flexAudioMute(), "a TX the hold did not unmute is not muted after");
}

// rnash2: in Hear both, a muted RX stays muted. Both now means both.
void testMonitorBothUnmutesRxForTheHold()
{
    SliceModel rx(0), tx(1);
    tx.setAudioMute(true);
    rx.setAudioMute(true);
    SplitMonitorHold hold;
    hold.begin(&rx, &tx, SplitAudioProfile::Monitor::Both);
    check(!rx.flexAudioMute() && !tx.flexAudioMute(), "both: both audible");
    hold.end(&rx, &tx);
    check(rx.flexAudioMute() && tx.flexAudioMute(), "both: both muted again");

    rx.setAudioMute(false);
    hold.begin(&rx, &tx, SplitAudioProfile::Monitor::Both);
    hold.end(&rx, &tx);
    check(!rx.flexAudioMute(), "both: an audible RX is left audible");
}

// Replacement taking over DURING the hold: release must not write into it.
void testMonitorReplacementMidHold()
{
    SliceModel rx(0), tx(1);
    tx.setAudioMute(true);
    SplitMonitorHold hold;
    hold.begin(&rx, &tx, SplitAudioProfile::Monitor::Solo);   // native RX muted
    rx.setExternalReceiveAudioReplacementMute(true);           // Kiwi takes over
    check(!rx.audioMute(), "setup: replacement audible");
    hold.end(&rx, &tx);
    check(!rx.audioMute(),
          "a replacement that began mid-hold is not muted by the release");
}

void testMonitorRefusesReplacedTxAndMissingSlices()
{
    SliceModel rx(0), tx(1);
    tx.setExternalReceiveAudioReplacementMute(true);
    SplitMonitorHold hold;
    check(!hold.begin(&rx, &tx, SplitAudioProfile::Monitor::Solo),
          "a replaced TX slice refuses the hold");
    check(!hold.active() && !rx.flexAudioMute(), "and writes nothing");
    check(!hold.begin(nullptr, &tx, SplitAudioProfile::Monitor::Solo),
          "no RX slice, no hold");

    SliceModel tx2(2);
    tx2.setAudioMute(true);
    hold.begin(&rx, &tx2, SplitAudioProfile::Monitor::Solo);
    hold.end(&rx, nullptr);      // the TX slice was removed mid-hold
    check(!rx.flexAudioMute(), "release with a removed TX still restores RX");
    check(!hold.active(), "and ends the hold");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testEmptyIsTodaysBehaviour();
    testTouchedIsDistinctFromDefaultValue();
    testFullRoundTrip();
    testVersionGate();
    testWrongTypeDropsLearnedStateButKeepsMonitor();
    testClamping();
    testForgetKeepsMonitorPreference();
    testUntouchedSplitLearnsNothing();
    testWorkedExample();
    testArrangementSurvivesUntouchedReplays();
    testTouchedFieldReplacesOnlyItself();
    testMutingOnceReturnsToTheOldBehaviour();
    testRxPanRestoresToTheTruePreSplitValue();
    testRecorderSurvivesTheSliceItDescribes();
    testDisarmedRecorderIgnoresNotes();
    testRecorderClamps();
    testNoRxSliceMeansNoRestore();
    testRepeatedSplitsOnRealSlices();
    testWritesOutsideTheOperatorScopeAreNotLearned();
    testMonitorLeavesReplacedRxAlone();
    testMonitorSoloRestoresExactly();
    testMonitorBothUnmutesRxForTheHold();
    testMonitorReplacementMidHold();
    testMonitorRefusesReplacedTxAndMissingSlices();
    testForgetMidSplitDropsPendingEditsKeepsRestore();
    testHoldRestoresOnlyIntoTheSameObjects();
    testHoldOnDestroyedSlicesWritesNothing();
    testHoldExposesItsObjectsForTheReclaimCheck();
    testKiwiTakeoverAfterEndingTheHoldRestoresAudible();

    if (g_failures) {
        std::fprintf(stderr, "split_audio_profile_test: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("split_audio_profile_test: all checks passed\n");
    return EXIT_SUCCESS;
}
