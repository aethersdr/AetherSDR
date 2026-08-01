// #4567 regression: TCI receiver numbers must survive a mid-session slice
// destroy/recreate. The captured failure: [A(id0)=trx0, B(id1)=trx1], band
// switch destroys+recreates A, positional numbering renumbered B to receiver
// 0 under a live client. With TciTrxMap, the recreated slice (same Flex id)
// reclaims its binding and surviving slices never move.
//
// Pure-logic test (no RadioModel): binding acquisition, recreate reuse,
// lowest-free assignment, release, clear, and the trx_count floor. The
// map↔model interplay (positional fallback, live-slice resolution) is
// exercised on hardware — see PR verification.

#include "core/TciTrxMap.h"

#include <iostream>

using AetherSDR::TciTrxMap;

namespace {

int failures = 0;

void expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    if (!condition)
        ++failures;
}

} // namespace

int main()
{
    // Creation order gets dense numbers from 0 — identical to the
    // positional policy for a session with no destroy/recreate.
    {
        TciTrxMap map;
        expect(map.acquire(0) == 0, "first slice binds trx 0");
        expect(map.acquire(1) == 1, "second slice binds trx 1");
        expect(map.acquire(0) == 0, "acquire is idempotent for a bound id");
        expect(map.trxCount(nullptr) == 2, "trx_count = highest bound + 1");
    }

    // The #4567 capture: recreate reclaims its number, survivor never moves.
    {
        TciTrxMap map;
        map.acquire(0);  // slice A
        map.acquire(1);  // slice B
        // Band switch: A removed (release deferred past the settle window —
        // the owner has NOT called release), then re-added with the same id.
        expect(map.acquire(0) == 0, "recreated slice reclaims its trx");
        expect(map.acquire(1) == 1, "surviving slice keeps its trx");
        expect(map.trxCount(nullptr) == 2, "count unchanged across recreate");
    }

    // A genuine close (deferred release fired) frees the number for reuse.
    {
        TciTrxMap map;
        map.acquire(0);
        map.acquire(1);
        map.release(0);
        expect(map.acquire(7) == 0, "freed trx is reused lowest-first");
        expect(map.acquire(1) == 1, "unrelated binding untouched by release");
        // A hole (release of trx 0 with trx 1 still bound) must not shrink
        // the advertised count below an index in use.
        map.release(7);
        expect(map.trxCount(nullptr) == 2,
               "count stays above a transient hole (trx 1 still bound)");
    }

    // Disconnect: everything resets; next session starts dense from 0.
    {
        TciTrxMap map;
        map.acquire(3);
        map.acquire(5);
        map.clear();
        expect(map.trxCount(nullptr) == 1, "cleared map advertises 1 (floor)");
        expect(map.acquire(5) == 0, "post-clear acquisition restarts at 0");
    }

    // Null-model lookups fall back safely (no map hit, no crash).
    {
        TciTrxMap map;
        expect(map.trxForSlice(nullptr, nullptr) == 0,
               "null slice falls back to positional default 0");
        expect(map.sliceForTrx(nullptr, 0) == nullptr,
               "null model resolves no slice");
        expect(map.txSliceTrxOrNone(nullptr) == -1,
               "null model has no TX slice (-1 sentinel)");
        expect(!map.trxHasLiveSlice(nullptr, 0),
               "null model carries no live slice");
    }

    std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures
              << " failures)\n";
    return failures ? 1 : 0;
}
