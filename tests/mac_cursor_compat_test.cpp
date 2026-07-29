#include "gui/MacCursorCompat.h"

#include <cstdio>

using AetherSDR::macSafeCursorShape;

namespace {

int g_failures = 0;

void expectShape(const char* name,
                 Qt::CursorShape actual,
                 Qt::CursorShape expected)
{
    const bool ok = actual == expected;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failures;
    }
}

} // namespace

int main()
{
#ifdef Q_OS_MAC
    expectShape("vertical split uses native vertical resize",
                macSafeCursorShape(Qt::SplitVCursor),
                Qt::SizeVerCursor);
    expectShape("horizontal split uses native horizontal resize",
                macSafeCursorShape(Qt::SplitHCursor),
                Qt::SizeHorCursor);
    expectShape("move cursor uses native open hand",
                macSafeCursorShape(Qt::SizeAllCursor),
                Qt::OpenHandCursor);
    expectShape("backward diagonal uses native horizontal resize",
                macSafeCursorShape(Qt::SizeBDiagCursor),
                Qt::SizeHorCursor);
    expectShape("forward diagonal uses native horizontal resize",
                macSafeCursorShape(Qt::SizeFDiagCursor),
                Qt::SizeHorCursor);
#else
    expectShape("vertical split remains unchanged",
                macSafeCursorShape(Qt::SplitVCursor),
                Qt::SplitVCursor);
    expectShape("horizontal split remains unchanged",
                macSafeCursorShape(Qt::SplitHCursor),
                Qt::SplitHCursor);
    expectShape("move cursor remains unchanged",
                macSafeCursorShape(Qt::SizeAllCursor),
                Qt::SizeAllCursor);
    expectShape("backward diagonal remains unchanged",
                macSafeCursorShape(Qt::SizeBDiagCursor),
                Qt::SizeBDiagCursor);
    expectShape("forward diagonal remains unchanged",
                macSafeCursorShape(Qt::SizeFDiagCursor),
                Qt::SizeFDiagCursor);
#endif

    expectShape("native arrow remains unchanged",
                macSafeCursorShape(Qt::ArrowCursor),
                Qt::ArrowCursor);

    std::printf("%s\n",
                g_failures == 0 ? "All tests passed." : "Test failures.");
    return g_failures == 0 ? 0 : 1;
}
