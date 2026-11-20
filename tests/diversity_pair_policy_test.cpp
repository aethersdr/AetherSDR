// Which two diversity slices form one receive pair (#4891). Cases use the
// slice status captured from a FLEX-8600 with two pairs up:
//   slice 0 (A) pan 0x40000000 parent  diversity_index=3
//   slice 3 (D) pan 0x40000000 child   diversity_index=0
//   slice 1 (B) pan 0x40000001 parent  diversity_index=2
//   slice 2 (C) pan 0x40000001 child   diversity_index=1
#include "models/DiversityPairPolicy.h"

#include <cstdio>

using namespace AetherSDR::DiversityPairPolicy;

namespace {

int g_failed = 0;

void expectPair(const char* name, const SliceView& a, const SliceView& b, bool want)
{
    const bool ab = isSamePair(a, b);
    const bool ba = isSamePair(b, a);
    const bool ok = ab == want && ba == want;
    std::printf("[%s] %s (want=%d got=%d/%d)\n", ok ? " OK " : "FAIL", name, want, ab, ba);
    if (!ok)
        ++g_failed;
}

SliceView slice(int id, bool parent, int index, const char* pan)
{
    SliceView v;
    v.sliceId = id;
    v.diversity = true;
    v.parent = parent;
    v.child = !parent;
    v.diversityIndex = index;
    v.panId = QString::fromLatin1(pan);
    return v;
}

constexpr int kJunk = 1342177293;  // tests/data/flex_ingress_corpus.txt

} // namespace

int main()
{
    const char* p1 = "0x40000000";
    const char* p2 = "0x40000001";
    const SliceView A = slice(0, true, 3, p1);
    const SliceView D = slice(3, false, 0, p1);
    const SliceView B = slice(1, true, 2, p2);
    const SliceView C = slice(2, false, 1, p2);

    // Captured status: the real pairs, and no cross-pan pairing (#4891).
    expectPair("log: A-D pair", A, D, true);
    expectPair("log: B-C pair", B, C, true);
    expectPair("log: A-C cross-pan parent/child", A, C, false);
    expectPair("log: B-D cross-pan parent/child", B, D, false);
    expectPair("log: A-B two parents", A, B, false);

    // Both pairs on one pan: the index still separates them.
    expectPair("same pan: A-C not a pair", A, slice(2, false, 1, p1), false);
    expectPair("same pan: B-D not a pair", slice(1, true, 2, p1), D, false);

    // No index yet: pan fallback.
    expectPair("no index: A-D same pan", slice(0, true, -1, p1), slice(3, false, -1, p1), true);
    expectPair("no index: A-C cross pan", slice(0, true, -1, p1), slice(2, false, -1, p2), false);
    expectPair("one side indexed: pan decides", A, slice(3, false, -1, p1), true);

    // Junk index (out of range) falls back to the pan test.
    expectPair("junk idx: A-D same pan", slice(0, true, kJunk, p1), slice(3, false, kJunk, p1), true);
    expectPair("junk idx: A-C cross pan", slice(0, true, kJunk, p1), slice(2, false, kJunk, p2), false);

    // No index and no pan yet: role-only answer.
    expectPair("no index, no pan: role-only keeps pair",
               slice(0, true, -1, ""), slice(3, false, -1, ""), true);

    // Diversity off on either side.
    SliceView off = D;
    off.diversity = false;
    expectPair("diversity off", A, off, false);

    if (g_failed == 0) {
        std::printf("\nAll diversity pair policy tests passed.\n");
        return 0;
    }
    std::printf("\n%d diversity pair policy test(s) failed.\n", g_failed);
    return 1;
}
