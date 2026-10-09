// Which slice a panadapter's title names (PanSliceTitle::pick, #6274).

#include "gui/PanSliceTitle.h"

#include <cstdio>

using AetherSDR::PanSliceTitle::SliceOnPan;
using AetherSDR::PanSliceTitle::pick;

static int g_failures = 0;

#define EXPECT_EQ(actual, expected) do { \
    const int a_ = (actual); const int e_ = (expected); \
    if (a_ != e_) { \
        std::fprintf(stderr, "FAIL %s:%d  expected %d, got %d\n", \
                     __FILE__, __LINE__, e_, a_); \
        ++g_failures; \
    } \
} while (0)

int main()
{
    const QString p1 = QStringLiteral("0x40000000");
    const QString p2 = QStringLiteral("0x40000001");

    // Active slice B sits second on the pan: it wins over the first slice.
    const QList<SliceOnPan> twoOnP1{{0, p1}, {1, p1}, {2, p2}};
    EXPECT_EQ(pick(p1, 1, 0, twoOnP1), 1);

    // Active slice is on another pan: keep the slice the title names.
    EXPECT_EQ(pick(p1, 2, 1, twoOnP1), 1);

    // The named slice left (moved or removed): fall back to the first here.
    const QList<SliceOnPan> moved{{0, p1}, {1, p2}, {2, p2}};
    EXPECT_EQ(pick(p1, 2, 1, moved), 0);

    // The active slice was removed and is no longer listed.
    EXPECT_EQ(pick(p1, 3, 3, twoOnP1), 0);

    // No slice left on the pan: the title is cleared.
    EXPECT_EQ(pick(p1, 2, 0, {{2, p2}}), -1);
    EXPECT_EQ(pick(p1, -1, -1, {}), -1);

    if (g_failures == 0)
        std::printf("pan_slice_title_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
