// VitaSequenceTracker: reordered packets are not losses, missing ones are.
// CMake target `vita_sequence_tracker_test`.  Exit 0 = pass.

#include "core/backends/flex/VitaSequenceTracker.h"

#include <cstdio>
#include <vector>

using AetherSDR::VitaSequenceTracker;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

struct Totals {
    int lost = 0;
    int breaks = 0;
    int reordered = 0;
    int duplicates = 0;
};

Totals feed(const std::vector<int>& seqs)
{
    VitaSequenceTracker t;
    Totals s;
    for (int q : seqs) {
        const auto r = t.add(q);
        s.lost += r.lost;
        s.breaks += r.breaks;
        s.reordered += r.reordered ? 1 : 0;
        s.duplicates += r.duplicate ? 1 : 0;
    }
    return s;
}

std::vector<int> repeat(const std::vector<int>& pattern, int times)
{
    std::vector<int> out;
    for (int i = 0; i < times; ++i) {
        out.insert(out.end(), pattern.begin(), pattern.end());
    }
    return out;
}

} // namespace

int main()
{
    // In order.
    std::vector<int> inOrder;
    for (int i = 0; i < 200; ++i) {
        inOrder.push_back(i & 0x0F);
    }
    Totals t = feed(inOrder);
    check(t.lost == 0 && t.reordered == 0, "in-order stream: no loss");

    // FFT as captured: each frame's two packets swapped (2,1,4,3,...).
    t = feed(repeat({2, 1, 4, 3, 6, 5, 8, 7, 10, 9, 12, 11, 14, 13, 0, 15}, 20));
    check(t.lost == 0, "swapped pairs (FFT): no loss");
    check(t.reordered == 160, "swapped pairs counted as reordered");

    // Waterfall as captured: the short last packet of a tile first.
    t = feed(repeat({5, 4, 1, 0, 2, 3, 11, 10, 9, 6, 7, 8, 12, 15, 14, 13}, 20));
    check(t.lost == 0, "waterfall tile reordering: no loss");

    // Real loss: every 10th packet dropped.
    std::vector<int> dropped;
    int expectLost = 0;
    for (int i = 0; i < 400; ++i) {
        if (i % 10 == 9) {
            ++expectLost;
            continue;
        }
        dropped.push_back(i & 0x0F);
    }
    dropped.push_back(400 & 0x0F);   // flush: later packets push the gaps out
    for (int i = 401; i < 410; ++i) {
        dropped.push_back(i & 0x0F);
    }
    t = feed(dropped);
    std::printf("  lost %d of %d dropped\n", t.lost, expectLost);
    check(t.lost == expectLost, "dropped packets all counted");

    // Loss and reordering together.
    t = feed({0, 1, 3, 2, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0});
    check(t.lost == 1 && t.reordered == 1, "one swap and one drop");

    // A big gap (9 lost) after the stream has settled counts at once.
    t = feed({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
              0, 1, 2, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7, 8});
    std::printf("  big gap lost %d\n", t.lost);
    check(t.lost == 9, "a 9-packet gap counts 9");

    // Duplicate of the newest packet.
    t = feed({0, 1, 1, 2, 3});
    check(t.lost == 0 && t.duplicates == 1, "duplicate is not a loss");

    // A gap is one break however many packets it held: the network-quality
    // input keeps the per-break counting its thresholds were tuned against.
    {
        std::vector<int> seqs;
        for (int i = 0; i < 40; ++i) seqs.push_back(i);
        for (int i = 46; i < 90; ++i) seqs.push_back(i);   // 40..45 lost: 6
        const Totals t = feed(seqs);
        check(t.lost == 6, "a 6-packet burst counts 6 lost");
        check(t.breaks == 1, "a 6-packet burst is one break");
    }
    {
        std::vector<int> seqs;
        for (int i = 0; i < 40; ++i) seqs.push_back(i);
        for (int i = 41; i < 60; ++i) seqs.push_back(i);   // 40 lost
        for (int i = 63; i < 90; ++i) seqs.push_back(i);   // 60..62 lost
        const Totals t = feed(seqs);
        check(t.lost == 4 && t.breaks == 2, "two separate gaps are two breaks");
    }
    {
        std::vector<int> seqs;
        for (int i = 0; i < 40; ++i) seqs.push_back(i);
        for (int i = 50; i < 90; ++i) seqs.push_back(i);   // 40..49 lost: a jump past the window
        const Totals t = feed(seqs);
        check(t.lost == 10 && t.breaks == 1, "a jump past the window is one break");
    }
    {
        // Reordering is no break.
        std::vector<int> seqs;
        for (int i = 0; i < 80; i += 2) { seqs.push_back(i + 1); seqs.push_back(i); }
        const Totals t = feed(seqs);
        check(t.breaks == 0, "swapped pairs are no break");
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
