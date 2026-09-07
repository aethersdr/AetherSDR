// Ownership of the Client-Side QSO recorder's TX slot (#2539, #4281).
//
// Two producers can reach QsoRecorder::feedTxAudio — the mic-driven monitor
// tap and the CW record pump — and the recorder writes whatever arrives while
// its TX gate is open. #4281: the pump additionally required the PC mic
// capture stream to be DOWN, but mic capture follows mic_selection == "PC" and
// stays up across mode changes, so on an ordinary client setup the pump never
// ran for the whole CW over while the mic tap kept writing. The recording
// carried room noise where the operator's CW should have been.
//
// The rule is now a single pure function, so both call sites ask one question
// and cannot drift apart. This pins it.

#include "core/CwRecordGate.h"

#include <cstdio>
#include <type_traits>

using namespace AetherSDR;

namespace {

bool expect(bool cond, const char* label)
{
    std::printf("%s %s\n", cond ? "[ OK ]" : "[FAIL]", label);
    return cond;
}

const char* name(TxRecorderSource s)
{
    switch (s) {
    case TxRecorderSource::None:       return "None";
    case TxRecorderSource::Mic:        return "Mic";
    case TxRecorderSource::CwSidetone: return "CwSidetone";
    }
    return "?";
}

} // namespace

// ── Contract pin ────────────────────────────────────────────────────────────
// The owner is a function of exactly two things: the radio's interlock TX
// state and whether OUR keyer fired this over. Re-introducing a third input —
// mic-capture state was the #4281 defect — fails the build here rather than
// silently restoring the bug, and does so on the contract rather than on the
// spelling of any line of source.
static_assert(std::is_same_v<decltype(txRecorderSource),
                             TxRecorderSource(bool, bool)>,
              "#4281: the recorder's TX-slot owner takes exactly "
              "(radioTransmitting, cwKeyedThisOver) — mic-capture state is not "
              "an input and must not become one");

// ── Truth table, verified at compile time as well as at run time ────────────
static_assert(txRecorderSource(false, false) == TxRecorderSource::None);
static_assert(txRecorderSource(true,  false) == TxRecorderSource::Mic);
static_assert(txRecorderSource(true,  true)  == TxRecorderSource::CwSidetone);
// ★ The row this fix turns over. It used to assert None, on the assumption that
// "not transmitting" means the over is finished. Break-in disproves that: the
// interlock is false in every inter-element gap while the over continues.
static_assert(txRecorderSource(false, true)  == TxRecorderSource::CwSidetone);

// ── Our transmission, not any client's (#4281 round 2) ─────────────────────
// The latch machinery previously consumed the raw any-owner interlock, which
// let a foreign transmission validate or prolong our over and let a tune
// carrier hold it open. cwOverTxActive is the composite every latch site now
// asks: transmitting AND attributed to us AND not a tune carrier. Full truth
// table — exactly one true row.
static_assert(!cwOverTxActive(false, false, false));
static_assert(!cwOverTxActive(false, false, true));
static_assert(!cwOverTxActive(false, true,  false));
static_assert(!cwOverTxActive(false, true,  true));
static_assert(!cwOverTxActive(true,  false, false),
              "#4281: another client's transmission is not our over");
static_assert(!cwOverTxActive(true,  false, true));
static_assert(!cwOverTxActive(true,  true,  true),
              "#4281: our own tune carrier is not our CW over");
static_assert( cwOverTxActive(true,  true,  false),
              "an attributed, non-tune transmission is ours");

// Rendering additionally requires an open file. Ownership stays a two-input
// contract (pinned above); "is there a file" is a separate question, so the
// defect that started this — an extra input folded into ownership — cannot
// come back through this door either.
static_assert(!cwRecordPumpShouldRender(TxRecorderSource::None,       true));
static_assert(!cwRecordPumpShouldRender(TxRecorderSource::Mic,        true));
static_assert( cwRecordPumpShouldRender(TxRecorderSource::CwSidetone, true));
static_assert(!cwRecordPumpShouldRender(TxRecorderSource::CwSidetone, false));
// A gap inside an over still renders — the generator emits silence there, and
// that silence is what preserves the morse spacing in the file.
static_assert( cwRecordPumpShouldRender(txRecorderSource(false, true), true));

int main()
{
    bool ok = true;

    // ── The #4281 regression itself ─────────────────────────────────────────
    // A CW over that our keyer is sending selects the pump. Before the fix the
    // pump also demanded !isTxStreaming(), so with the PC mic open — the
    // default client setup, and what the reporter's own support bundle shows
    // for the full session — this case never selected the pump at all.
    {
        const auto s = txRecorderSource(/*radioTransmitting=*/true,
                                        /*cwKeyedThisOver=*/true);
        ok &= expect(s == TxRecorderSource::CwSidetone,
                     "#4281: our CW over selects the record pump");
        ok &= expect(cwRecordPumpOwnsRecorder(s),
                     "#4281: the pump feeds the recorder during our CW over");
        ok &= expect(!micTapOwnsRecorder(s),
                     "#4281: the mic tap does NOT feed the recorder during our CW over");
    }

    // ── #3556 must not regress: a voice over still records the mic ──────────
    {
        const auto s = txRecorderSource(true, false);
        ok &= expect(s == TxRecorderSource::Mic,
                     "#3556: a phone/SSB over selects the mic monitor tap");
        ok &= expect(micTapOwnsRecorder(s),
                     "#3556: the mic tap feeds the recorder during a voice over");
        ok &= expect(!cwRecordPumpOwnsRecorder(s),
                     "a voice over does not start the CW record pump");
    }

    // ── Not transmitting ────────────────────────────────────────────────────
    // The pump must be idle. The mic tap is left un-suppressed on purpose: the
    // recorder's own gate fast-returns when not transmitting, so suppressing it
    // here as well would only duplicate that check in a second place.
    {
        ok &= expect(txRecorderSource(false, false) == TxRecorderSource::None,
                     "receive: no producer owns the recorder's TX slot");
        ok &= expect(!cwRecordPumpOwnsRecorder(txRecorderSource(false, false)),
                     "receive: the CW record pump stays idle");
        ok &= expect(micTapOwnsRecorder(txRecorderSource(true, false)),
                     "voice over: the mic tap owns the slot");
        // The latch is NOT stale during an over — the interlock is simply low
        // between elements. The pump must keep ownership across that gap, or
        // the mic tap fills it and the morse spacing is lost. Staleness is
        // handled by ageing the latch (cwLatchShouldAge, below), which needs
        // the hang elapsed AND the radio at RX — the interlock alone never
        // ends an over.
        ok &= expect(cwRecordPumpOwnsRecorder(txRecorderSource(false, true)),
                     "#4281: the pump keeps the slot through an inter-element gap");
        ok &= expect(!micTapOwnsRecorder(txRecorderSource(false, true)),
                     "#4281: the mic tap is blocked through an inter-element gap");
    }

    // ── The render gate: own the slot AND have somewhere to put it ─────────
    // With no file open QsoRecorder::feedTxAudio discards every block, so
    // rendering is waste on the audio thread. This must not disturb the gate
    // SIGNAL, which is what starts an auto-record — that ordering lives in
    // onCwRecordPump and is not covered by this pure test; the runtime check
    // is: auto-record armed, no file open, key a CW over, a file must open.
    {
        const auto cw = txRecorderSource(true, true);
        ok &= expect(cwRecordPumpShouldRender(cw, true),
                     "render: our CW over with a recording open");
        ok &= expect(!cwRecordPumpShouldRender(cw, false),
                     "no render: our CW over with NO recording open");
        ok &= expect(!cwRecordPumpShouldRender(txRecorderSource(true, false), true),
                     "no render: a voice over never drives the CW pump");
        ok &= expect(!cwRecordPumpShouldRender(txRecorderSource(false, false), true),
                     "no render: truly receiving, even with a recording open");
        ok &= expect(cwRecordPumpShouldRender(txRecorderSource(false, true), true),
                     "#4281: DO render through a gap — the silence is the spacing");

        // Rendering implies ownership, never the reverse.
        bool implies = true;
        for (int i = 0; i < 8; ++i) {
            const auto s2 = txRecorderSource((i & 1) != 0, (i & 2) != 0);
            if (cwRecordPumpShouldRender(s2, (i & 4) != 0)
                && !cwRecordPumpOwnsRecorder(s2))
                implies = false;
        }
        ok &= expect(implies, "render is only ever a narrowing of ownership");
    }

    // ── Exactly one producer, for every reachable input ─────────────────────
    // This is the property the recorder depends on and cannot check for itself:
    // it writes whatever arrives. If both predicates were ever true together
    // the WAV would interleave mic and sidetone block by block.
    // The full ownership truth table, stated explicitly. This replaces an
    // earlier "exactly one owner" loop that compared cwRecordPumpOwnsRecorder
    // against micTapOwnsRecorder — since those are defined as (s == CwSidetone)
    // and (s != CwSidetone), that assertion reduced to x != !x and could not
    // fail for any input. An exhaustive table cannot go vacuous the same way.
    {
        struct Row { bool tx; bool cw; TxRecorderSource want; const char* why; };
        static constexpr Row kRows[] = {
            {false, false, TxRecorderSource::None,
             "receiving, nothing keyed"},
            {true,  false, TxRecorderSource::Mic,
             "#3556: a voice over is the mic's"},
            {true,  true,  TxRecorderSource::CwSidetone,
             "#4281: our CW over is the pump's"},
            {false, true,  TxRecorderSource::CwSidetone,
             "#4281: an inter-element gap is still our over"},
        };
        bool table = true;
        for (const Row& r : kRows) {
            const auto got = txRecorderSource(r.tx, r.cw);
            if (got != r.want) {
                table = false;
                std::printf("  tx=%d cw=%d -> %s, wanted %s (%s)\n",
                            r.tx ? 1 : 0, r.cw ? 1 : 0,
                            name(got), name(r.want), r.why);
            }
        }
        ok &= expect(table, "ownership truth table holds for all four states");
    }

    // #4281: the over-hang is what holds RX audio off after the last element, so
    // its LENGTH is correctness, not tuning — every extra millisecond is a
    // millisecond of the other station's reply missing from the recording.
    {
        static_assert(cwOverHangMs(20) == 480,
                      "#4281: 8 dit units at 20 WPM is 480 ms");
        static_assert(cwOverHangMs(30) == 320, "8 units at 30 WPM");
        static_assert(cwOverHangMs(0)  == cwOverHangMs(20),
                      "a nonsense speed falls back to 20 WPM, never divides by zero");
        // It must outlast the longest silence inside an over (the inter-word
        // gap, 7 units), or an over splits mid-transmission. Hang and gap are
        // the same 1200/wpm unit scaled, so this is a property of the unit
        // count, not of any speed — pinned on the constant rather than looped
        // over speeds, which could not fail for any wpm.
        static_assert(kCwOverHangUnits > 7,
                      "#4281: the over-hang must outlast a 7-unit inter-word gap");
        // ...and must NOT be the old fixed 1500 ms, which held RX off for 1.5 s
        // after every over regardless of speed.
        ok &= expect(cwOverHangMs(20) < 1500,
                     "#4281: the hang is far shorter than the 1500 ms first attempt");

        // ── The over-scoped speed override (#4281 round 2) ──────────────────
        // CWX keys at CwxModel's own per-segment speed, which the
        // TransmitModel::cwSpeed mirror never sees. The reviewer's scenario,
        // by the arithmetic: paddle 30 WPM → hang 8×40 = 320 ms; CWX macro at
        // 15 WPM → inter-word gap 7×80 = 560 ms > 320, so under break-in the
        // latch aged inside every word gap and the over split per word. The
        // override carries the over's slowest announced speed; the SLOWER
        // speed (longer hang) always wins, because a hang too short splits
        // the over while a hang too long costs bounded tail milliseconds.
        static_assert(cwOverHangMs(30, 0) == cwOverHangMs(30),
                      "no override (a paddle over): the mirror alone decides");
        static_assert(cwOverHangMs(30, 15) == cwOverHangMs(15),
                      "#4281: a slower CWX message re-sizes the hang");
        static_assert(cwOverHangMs(30, 15) == 640,
                      "#4281: 640 ms outlasts the 15 WPM word gap of 560 ms");
        static_assert(cwOverHangMs(15, 30) == cwOverHangMs(15),
                      "a FASTER override never shortens the hang — min wins");
        static_assert(cwOverHangMs(0, 15) == cwOverHangMs(15),
                      "nonsense mirror with a real override: the override decides");
        ok &= expect(cwOverHangMs(30, 15) > 7 * 1200 / 15,
                     "#4281: the overridden hang outlasts the slow inter-word gap");
        ok &= expect(cwOverHangMs(30, 26) == cwOverHangMs(26),
                     "#4281: even a modest speed split (30/26) takes the slower hang");
    }

    // ── The latch ages only while no transmission of OURS is up ─────────────
    // The stopwatch alone used to end the over. Whenever the radio was still
    // holding TX past the hang — break-in off, or a break-in delay longer than
    // the hang — the aged-out latch handed the slot to the mic tap in the
    // middle of our over. The our-TX term is a required SECOND condition, not
    // a replacement: under break-in it is false in every gap, so the stopwatch
    // is still what ends the over there. Round 2: the term is cwOverTxActive,
    // not the raw interlock — a transmission that is not ours no longer holds
    // the over open.
    {
        static_assert(cwLatchShouldAge(/*ourTx*/ false, /*gap*/ 481, /*hang*/ 480),
                      "no TX of ours and the hang elapsed: the over is finished");
        static_assert(!cwLatchShouldAge(/*ourTx*/ true, /*gap*/ 481, /*hang*/ 480),
                      "#4281: our TX still holds — the over is NOT finished, "
                      "whatever the stopwatch says");
        static_assert(!cwLatchShouldAge(false, 479, 480),
                      "inside the hang: an inter-word gap is still the over");
        ok &= expect(cwLatchShouldAge(false, 481, 480),
                     "latch ages once our TX is down and the hang has elapsed");
        ok &= expect(!cwLatchShouldAge(true, 481, 480),
                     "#4281: latch holds while our transmission holds, past the hang");
        ok &= expect(!cwLatchShouldAge(true, 100000, 480),
                     "#4281: ...for as long as our TX holds — no wall-clock ceiling");
        ok &= expect(!cwLatchShouldAge(false, 480, 480),
                     "exactly the hang is not yet past it");
        ok &= expect(!cwLatchShouldAge(false, 0, 480),
                     "a fresh key edge never ages");

        // The composite feeding that term (#4281 round 2): what is NOT ours
        // does not hold the over, however long it transmits.
        static_assert(cwLatchShouldAge(cwOverTxActive(true, false, false), 481, 480),
                      "a foreign transmission past the hang does not hold our over");
        static_assert(cwLatchShouldAge(cwOverTxActive(true, true, true), 481, 480),
                      "a tune carrier past the hang does not hold the over");
        static_assert(!cwLatchShouldAge(cwOverTxActive(true, true, false), 481, 480),
                      "#4281: our own attributed TX still holds the over");
        ok &= expect(cwLatchShouldAge(cwOverTxActive(true, false, false), 481, 480),
                     "another client's TX does not hold our over past the hang");
        ok &= expect(cwLatchShouldAge(cwOverTxActive(true, true, true), 481, 480),
                     "TUNE raised within the hang no longer holds the over open");
        ok &= expect(!cwLatchShouldAge(cwOverTxActive(true, true, false), 100000, 480),
                     "#4281: our attributed TX holds — still no wall-clock ceiling");
    }

    // ── The idle countdown arms only when BOTH over sources are down ────────
    // The CW gate-close is queued and the over-hang lets a voice over begin
    // before it lands: arming the countdown inside that live voice over would
    // auto-stop the recording mid-transmission. Whichever over ends last arms.
    {
        static_assert(!idleCountdownShouldArm(true, /*tx*/ true, /*cw*/ false),
                      "CW gate-close inside a live voice over must not arm");
        static_assert(!idleCountdownShouldArm(true, /*tx*/ false, /*cw*/ true),
                      "MOX drop during a still-active CW over must not arm");
        static_assert(idleCountdownShouldArm(true, false, false),
                      "the last over source to end arms the countdown");
        ok &= expect(!idleCountdownShouldArm(true, true, false),
                     "#4281: CW gate-close inside a live voice over does not arm the idle stop");
        ok &= expect(!idleCountdownShouldArm(true, false, true),
                     "MOX drop during an active CW over does not arm the idle stop");
        ok &= expect(idleCountdownShouldArm(true, false, false),
                     "both over sources down: the idle countdown arms");
        ok &= expect(!idleCountdownShouldArm(false, false, false),
                     "no recording open: nothing to arm");
    }

    return ok ? 0 : 1;
}
