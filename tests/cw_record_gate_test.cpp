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
static_assert(txRecorderSource(false, true)  == TxRecorderSource::None);
static_assert(txRecorderSource(true,  false) == TxRecorderSource::Mic);
static_assert(txRecorderSource(true,  true)  == TxRecorderSource::CwSidetone);

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
        ok &= expect(!cwRecordPumpOwnsRecorder(txRecorderSource(false, true)),
                     "a stale CW latch cannot run the pump while receiving");
    }

    // ── Exactly one producer, for every reachable input ─────────────────────
    // This is the property the recorder depends on and cannot check for itself:
    // it writes whatever arrives. If both predicates were ever true together
    // the WAV would interleave mic and sidetone block by block.
    {
        bool exclusive = true;
        for (int i = 0; i < 4; ++i) {
            const bool tx = (i & 1) != 0;
            const bool cw = (i & 2) != 0;
            const auto s = txRecorderSource(tx, cw);
            const bool pump = cwRecordPumpOwnsRecorder(s);
            const bool mic  = micTapOwnsRecorder(s);
            if (pump == mic) {
                exclusive = false;
                std::printf("  tx=%d cw=%d -> %s  pump=%d mic=%d\n",
                            tx ? 1 : 0, cw ? 1 : 0, name(s),
                            pump ? 1 : 0, mic ? 1 : 0);
            }
        }
        ok &= expect(exclusive,
                     "exactly one producer owns the recorder's TX slot in every state");
    }

    return ok ? 0 : 1;
}
