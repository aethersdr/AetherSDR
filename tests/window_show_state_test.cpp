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
// windowIsShowing() fixes (1) and showAndRaiseWindow() fixes (2); clearing only
// the Minimized bit (not showNormal()) keeps a maximized or fullscreen window in
// that state across the reopen (#3918).
//
// Runs on the offscreen platform, where both behaviours reproduce.

#include "gui/WindowShowState.h"

#include <QApplication>
#include <QDialog>
#include <QMenu>
#include <QWidget>

#include <cstdio>

using AetherSDR::showAndRaiseWindow;
using AetherSDR::WindowMenuEntry;
using AetherSDR::windowInventory;
using AetherSDR::windowMenuLabel;
using AetherSDR::windowIsShowing;
using AetherSDR::windowTypeAppearsInMenu;

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
    if (windowIsShowing(w)) {
        w->hide();
    } else {
        showAndRaiseWindow(w);
    }
}

const WindowMenuEntry* entryFor(const QList<WindowMenuEntry>& entries,
                                const QWidget* window)
{
    for (const WindowMenuEntry& entry : entries) {
        if (entry.window == window) {
            return &entry;
        }
    }
    return nullptr;
}

int entryIndex(const QList<WindowMenuEntry>& entries, const QWidget* window)
{
    for (int i = 0; i < entries.size(); ++i) {
        if (entries.at(i).window == window) {
            return i;
        }
    }
    return -1;
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

    // --- Case 7: raising a maximized window must not drop it out of
    // maximized state (#3918).
    w.showMaximized();
    app.processEvents();
    if (w.isMaximized()) {
        showAndRaiseWindow(&w);
        app.processEvents();
        report("showAndRaiseWindow keeps a maximized window maximized",
               w.isMaximized());

        // --- Case 8: the reopen path itself must not drop maximize.  This is
        // what showNormal() got wrong: it clears Minimized AND Maximized, so a
        // maximized strip that was minimized came back at normal size.
        w.showMinimized();
        app.processEvents();
        report("Qt: minimizing keeps the Maximized bit alongside Minimized",
               w.isMinimized() && (w.windowState() & Qt::WindowMaximized));
        toggle(&w);
        app.processEvents();
        report("toggle restores a minimized-from-maximized window",
               windowIsShowing(&w));
        report("restored window is still maximized", w.isMaximized());
    } else {
        // Cases 7-8 are the ONLY coverage of maximize preservation in this
        // file, so a platform that declines to maximize leaves that guard
        // unverified here (the offscreen platform on Linux, macOS and Windows
        // does honour it).  Say so loudly rather than fail on a platform quirk.
        std::printf("[SKIP] maximized state unavailable on this platform - "
                    "maximize preservation (#3918) NOT verified by this run\n");
    }

    // --- Case 9: the same for FullScreen, which showNormal() also clears.
    w.showFullScreen();
    app.processEvents();
    if (w.isFullScreen()) {
        w.showMinimized();
        app.processEvents();
        report("Qt: minimizing keeps the FullScreen bit alongside Minimized",
               w.isMinimized() && (w.windowState() & Qt::WindowFullScreen));
        toggle(&w);
        app.processEvents();
        report("toggle restores a minimized-from-fullscreen window",
               windowIsShowing(&w));
        report("restored window is still fullscreen", w.isFullScreen());
    } else {
        std::printf("[SKIP] fullscreen state unavailable on this platform - "
                    "fullscreen preservation NOT verified by this run\n");
    }

    // --- Case 10: the Window menu inventory is automatic.  Any visible
    // titled top-level QWidget joins without a MainWindow registry entry.
    w.hide();
    QWidget mainWindow;
    mainWindow.setWindowTitle(QStringLiteral("AetherSDR"));
    mainWindow.show();
    QDialog aetherControl(&mainWindow);
    aetherControl.setWindowTitle(QStringLiteral("AetherControl"));
    aetherControl.show();
    QDialog pskReporter(&mainWindow);
    pskReporter.setWindowTitle(QStringLiteral("PSK Reporter"));
    pskReporter.showMinimized();
    QWidget hiddenWindow;
    hiddenWindow.setWindowTitle(QStringLiteral("Hidden Window"));
    QMenu transientMenu;
    transientMenu.setTitle(QStringLiteral("Transient Menu"));
    transientMenu.show();
    QWidget tooltip(nullptr, Qt::ToolTip);
    tooltip.show();
    QWidget splash(nullptr, Qt::SplashScreen);
    splash.show();
    constexpr Qt::WindowType kDesktopWindowType =
        static_cast<Qt::WindowType>(static_cast<int>(Qt::Window) | 0x10);
    app.processEvents();

    const QList<WindowMenuEntry> windows = windowInventory(&mainWindow);
    report("Window menu puts the primary window first",
           !windows.isEmpty() && windows.first().window == &mainWindow);
    report("Window menu discovers a visible modeless dialog",
           entryFor(windows, &aetherControl));
    report("Window menu keeps minimized windows reachable",
           entryFor(windows, &pskReporter));
    const WindowMenuEntry* pskEntry = entryFor(windows, &pskReporter);
    report("Window menu labels minimized windows",
           pskEntry && pskEntry->menuText == QStringLiteral("PSK Reporter — Minimized"));
    report("Window menu excludes hidden windows",
           !entryFor(windows, &hiddenWindow));
    report("Window menu excludes transient popup menus",
           !windowTypeAppearsInMenu(Qt::Popup)
               && !entryFor(windows, &transientMenu));
    report("Window menu excludes tooltips",
           !entryFor(windows, &tooltip));
    report("Window menu excludes splash screens",
           !entryFor(windows, &splash));
    report("Window menu excludes subwindows",
           !windowTypeAppearsInMenu(Qt::SubWindow));
    report("Window menu excludes desktop surfaces",
           !windowTypeAppearsInMenu(kDesktopWindowType));
    report("Window menu sorts secondary windows by title",
           entryIndex(windows, &aetherControl)
               < entryIndex(windows, &pskReporter));

    // topLevelWidgets() is unordered, not construction-ordered. Supply a
    // deliberately reversed list so deleting the sort fails on every run.
    const QList<WindowMenuEntry> ordered = windowInventory(
        {&pskReporter, &aetherControl, &mainWindow}, &mainWindow);
    report("Window menu orders explicit candidates primary-first then by title",
           ordered.size() == 3 && ordered.at(0).window == &mainWindow
               && ordered.at(1).window == &aetherControl
               && ordered.at(2).window == &pskReporter);

    // --- Case 11: a rare untitled real window is still reachable by an
    // object/class-name fallback rather than silently disappearing.
    QWidget unusualWindow;
    unusualWindow.setObjectName(QStringLiteral("namedTool"));
    unusualWindow.show();
    QDialog classFallback;
    classFallback.show();
    QWidget modifiedTitle;
    modifiedTitle.setWindowTitle(QStringLiteral("Audio & DSP[*]"));
    modifiedTitle.show();
    QWidget duplicateA;
    duplicateA.setObjectName(QStringLiteral("duplicateA"));
    duplicateA.setWindowTitle(QStringLiteral("Canvas & Tools"));
    duplicateA.show();
    QWidget duplicateB;
    duplicateB.setObjectName(QStringLiteral("duplicateB"));
    duplicateB.setWindowTitle(QStringLiteral("Canvas & Tools"));
    duplicateB.show();
    app.processEvents();
    report("Window menu labels untitled windows by object name",
           windowMenuLabel(&unusualWindow) == QStringLiteral("namedTool"));
    report("Window menu falls back to the widget class name",
           windowMenuLabel(&classFallback) == QStringLiteral("QDialog"));
    report("Window menu strips the modified-title placeholder",
           windowMenuLabel(&modifiedTitle) == QStringLiteral("Audio & DSP"));

    const QList<WindowMenuEntry> labelled = windowInventory(&mainWindow);
    report("Window menu retains an untitled user-facing window",
           entryFor(labelled, &unusualWindow));
    const WindowMenuEntry* modifiedEntry = entryFor(labelled, &modifiedTitle);
    report("Window menu escapes title ampersands",
           modifiedEntry
               && modifiedEntry->menuText == QStringLiteral("Audio && DSP"));
    const WindowMenuEntry* duplicateAEntry = entryFor(labelled, &duplicateA);
    const WindowMenuEntry* duplicateBEntry = entryFor(labelled, &duplicateB);
    report("Window menu numbers duplicate titles after sorting",
           duplicateAEntry && duplicateBEntry
               && duplicateAEntry->menuText == QStringLiteral("Canvas && Tools (1)")
               && duplicateBEntry->menuText == QStringLiteral("Canvas && Tools (2)"));

    if (g_failures == 0) {
        std::printf("All window show-state tests passed.\n");
    }
    return g_failures == 0 ? 0 : 1;
}
