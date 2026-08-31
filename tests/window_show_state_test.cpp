// Regression test for the Aetherial Audio Channel Strip toggle, which could
// not reopen the window once it had been minimized.
//
// Two Qt behaviours combine into the bug, and both are pinned here against a
// REAL QWidget rather than asserted from memory:
//
//   1. QWidget::isVisible() stays TRUE while a window is minimized.  A toggle
//      written `if (w->isVisible()) w->hide(); else w->show();` therefore
//      treats a minimized window as "showing" and hides it.
//   2. QWidget::show() on a minimized window restores its SAVED state, which
//      is still minimized — so the follow-up press does not recover it either.
//
// windowIsShowing() fixes (1) and showAndRaiseWindow() fixes (2), while the
// isMinimized() guard inside it keeps a maximized window maximized (#3918).
//
// Runs on the offscreen platform, where both behaviours reproduce.

#include "gui/WindowShowState.h"

#include <QApplication>
#include <QWidget>

#include <cstdio>

using AetherSDR::showAndRaiseWindow;
using AetherSDR::windowIsShowing;

namespace {

int g_failures = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failures;
    }
}

// The toggle exactly as MainWindow::toggleAetherialStrip() runs it.
void toggle(QWidget* w)
{
    if (windowIsShowing(w))
        w->hide();
    else
        showAndRaiseWindow(w);
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    // --- Case 1: null is never "showing", and raising it must not crash.
    report("windowIsShowing(nullptr) is false", !windowIsShowing(nullptr));
    showAndRaiseWindow(nullptr);
    report("showAndRaiseWindow(nullptr) is a no-op", true);

    QWidget w;
    w.resize(320, 240);

    // --- Case 2: a hidden window is not showing; the toggle opens it.
    report("hidden window is not showing", !windowIsShowing(&w));
    toggle(&w);
    app.processEvents();
    report("toggle opens a hidden window", windowIsShowing(&w));

    // --- Case 3: the toggle closes an ordinary open window.
    toggle(&w);
    app.processEvents();
    report("toggle hides a showing window", !w.isVisible());

    // --- Case 4: THE BUG.  Qt reports a minimized window as visible, so the
    // old bare isVisible() check would take the hide() branch here.
    w.show();
    app.processEvents();
    w.showMinimized();
    app.processEvents();
    report("Qt: minimized window still reports isVisible()", w.isVisible());
    report("Qt: minimized window reports isMinimized()", w.isMinimized());
    report("windowIsShowing() treats minimized as not showing",
           !windowIsShowing(&w));

    // --- Case 5: why the second press never recovered it either — show() on a
    // minimized window leaves it minimized.
    w.show();
    app.processEvents();
    report("Qt: show() does not un-minimize", w.isMinimized());

    // --- Case 6: the toggle restores a minimized window in ONE press.
    toggle(&w);
    app.processEvents();
    report("toggle restores a minimized window", windowIsShowing(&w));
    report("restored window is no longer minimized", !w.isMinimized());

    // --- Case 7: the isMinimized() guard — raising a maximized window must
    // not drop it out of maximized state (#3918).
    w.showMaximized();
    app.processEvents();
    if (w.isMaximized()) {
        showAndRaiseWindow(&w);
        app.processEvents();
        report("showAndRaiseWindow keeps a maximized window maximized",
               w.isMaximized());
    } else {
        // Some platforms decline to maximize; the guard is still pinned by
        // case 6, so skip rather than fail on a platform quirk.
        std::printf("[SKIP] maximized state unavailable on this platform\n");
    }

    if (g_failures == 0) {
        std::printf("All window show-state tests passed.\n");
    }
    return g_failures == 0 ? 0 : 1;
}
