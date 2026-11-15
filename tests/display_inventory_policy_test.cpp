// Unit tests for DisplayInventoryPolicy (#3856 Layer B).
//
// The radio-authoritative display inventory classifies every pan/waterfall the
// radio reports against what this client owns, and flags the #3843 fingerprint:
// a waterfall the radio still holds whose parent panadapter is gone (a leaked
// display resource). These tests pin the classification so a real leak can't be
// silently reported as clean and normal multi-client state isn't mis-flagged.

#include "models/DisplayInventoryPolicy.h"

#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::DisplayInventory;

namespace {

int failures = 0;

void check(bool condition, const char* name)
{
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

// Helper: does the leaked list contain id?
bool leaked(const Report& r, const QString& id)
{
    return r.leakedWaterfalls.contains(id);
}

// Healthy single-pan session: one owned pan + its waterfall, parent present.
void testCleanSinglePan()
{
    Inputs in;
    in.ourHandle = 0x11111111u;
    in.radioPans = {{"0x40000000", 0x11111111u}};
    in.radioWaterfalls = {{"0x42000000", 0x11111111u, "0x40000000"}};
    in.ownedPanIds = {"0x40000000"};
    in.ownedWaterfallIds = {"0x42000000"};

    const Report r = classify(in);
    check(r.oursPanCount == 1 && r.oursWfCount == 1, "clean: both ours");
    check(r.orphanPanCount == 0 && r.orphanWfCount == 0, "clean: no orphans");
    check(r.leakedWaterfalls.isEmpty(), "clean: no leaks");
}

// The #3843 leak: pan removed (gone from radio), waterfall lingers with a parent
// that no longer exists, and we no longer own it.
void testLeakedWaterfallParentGone()
{
    Inputs in;
    in.ourHandle = 0x11111111u;
    in.radioPans = {{"0x40000000", 0x11111111u}};           // only the first pan survives
    in.radioWaterfalls = {
        {"0x42000000", 0x11111111u, "0x40000000"},          // healthy, parent present
        {"0x42000001", 0x11111111u, "0x40000001"},          // LEAK: parent 0x40000001 gone
    };
    in.ownedPanIds = {"0x40000000"};
    in.ownedWaterfallIds = {"0x42000000"};                  // we no longer hold the leaked wf

    const Report r = classify(in);
    check(leaked(r, "0x42000001"), "leak: parent-gone waterfall flagged");
    check(!leaked(r, "0x42000000"), "leak: healthy waterfall not flagged");
    check(r.leakedWaterfalls.size() == 1, "leak: exactly one leak");
    check(r.orphanWfCount == 1, "leak: lingering wf is orphan (not ours/foreign)");
}

// A foreign Multi-Flex client's pan + waterfall must classify as foreign, not
// orphan, and a foreign waterfall with its own parent present is not a leak.
void testForeignNotOrphan()
{
    Inputs in;
    in.ourHandle = 0x11111111u;
    in.radioPans = {
        {"0x40000000", 0x11111111u},   // ours
        {"0x40000005", 0x22222222u},   // foreign
    };
    in.radioWaterfalls = {
        {"0x42000000", 0x11111111u, "0x40000000"},
        {"0x42000005", 0x22222222u, "0x40000005"},
    };
    in.ownedPanIds = {"0x40000000"};
    in.ownedWaterfallIds = {"0x42000000"};

    const Report r = classify(in);
    check(r.foreignPanCount == 1 && r.foreignWfCount == 1, "foreign: counted as foreign");
    check(r.orphanPanCount == 0 && r.orphanWfCount == 0, "foreign: not orphan");
    check(r.leakedWaterfalls.isEmpty(), "foreign: own parent present, no leak");
}

// A waterfall with no parent reported (empty parentPanId) can't be proven leaked,
// so it must NOT be flagged (avoid false positives on incomplete status).
void testNoParentReportedNotLeaked()
{
    Inputs in;
    in.ourHandle = 0x11111111u;
    in.radioPans = {{"0x40000000", 0x11111111u}};
    in.radioWaterfalls = {{"0x42000000", 0x11111111u, ""}};  // parent unknown
    in.ownedPanIds = {"0x40000000"};
    in.ownedWaterfallIds = {"0x42000000"};

    const Report r = classify(in);
    check(r.leakedWaterfalls.isEmpty(), "no-parent: not flagged as leak");
}

// Without knowing our own handle, a handle mismatch is unprovable → Orphan, not
// Foreign (we must not assert another client owns it).
void testUnknownOurHandleFallsToOrphan()
{
    Inputs in;
    in.ourHandle = 0;   // unknown
    in.radioPans = {{"0x40000005", 0x22222222u}};
    in.radioWaterfalls = {};
    // we own nothing
    const Report r = classify(in);
    check(r.orphanPanCount == 1 && r.foreignPanCount == 0,
          "unknown-handle: mismatch is orphan, not foreign");
}

}  // namespace

int main()
{
    testCleanSinglePan();
    testLeakedWaterfallParentGone();
    testForeignNotOrphan();
    testNoParentReportedNotLeaked();
    testUnknownOurHandleFallsToOrphan();

    if (failures == 0) {
        std::printf("\nAll display_inventory_policy tests passed.\n");
        return 0;
    }
    std::printf("\n%d display_inventory_policy test(s) FAILED.\n", failures);
    return 1;
}
