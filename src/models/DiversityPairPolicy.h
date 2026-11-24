#pragma once

#include <QString>

// Which two diversity slices form one receive pair (#4891). Pure policy:
// MainWindow::isSameDiversityReceivePair() feeds it two slice snapshots.

namespace AetherSDR::DiversityPairPolicy {

// A Flex radio has at most 8 slices, so a diversity_index outside 0..7 is
// not a slice index (the ingress corpus has 1342177293 on a diversity=0
// slice) and is treated as unknown.
inline constexpr int kMaxSlices = 8;

struct SliceView {
    int sliceId = -1;
    bool diversity = false;
    bool parent = false;
    bool child = false;
    int diversityIndex = -1;
    QString panId;
};

inline bool usableIndex(int index) { return index >= 0 && index < kMaxSlices; }

// The caller has already rejected a slice paired with itself.
inline bool isSamePair(const SliceView& a, const SliceView& b)
{
    if (!a.diversity || !b.diversity)
        return false;

    // diversity_index is the partner's slice index while diversity is on
    // (FlexLib Slice.cs DiversityIndex). When both slices carry a usable
    // one it decides.
    if (usableIndex(a.diversityIndex) && usableIndex(b.diversityIndex))
        return a.diversityIndex == b.sliceId && b.diversityIndex == a.sliceId;

    // No index yet: a parent and its child share one pan, so a parent/child
    // match on two different pans is two pairs (#4891). While a pan id is
    // unknown, fall back to the role test alone.
    const bool aPanKnown = !a.panId.isEmpty();
    const bool bPanKnown = !b.panId.isEmpty();
    const bool samePan = aPanKnown && bPanKnown && a.panId == b.panId;
    const bool parentChild = (a.parent && b.child) || (a.child && b.parent);
    if (parentChild)
        return samePan || !aPanKnown || !bPanKnown;
    return samePan;
}

} // namespace AetherSDR::DiversityPairPolicy
