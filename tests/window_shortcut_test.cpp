#include "TestSettingsProfile.h"
#include "core/ShortcutManager.h"

#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QSlider>
#include <QTest>
#include <QWidget>

#include <cstdio>

using AetherSDR::ShortcutManager;

namespace {
int failures = 0;

void expect(bool ok, const char* label)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", label);
    if (!ok) {
        ++failures;
    }
}

void focus(QWidget& window, QWidget* child = nullptr)
{
    window.show();
    window.raise();
    window.activateWindow();
    expect(QTest::qWaitForWindowActive(&window), "requested window became active");
    (child ? child : &window)->setFocus();
    QApplication::processEvents();
}

void press(Qt::Key key)
{
    QWidget* target = QApplication::focusWidget();
    expect(target != nullptr, "key event has a focus target");
    if (target) {
        QTest::keyClick(target, key);
    }
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("aether-window-shortcut-test"));
    if (!settings.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    QWidget mainWindow;
    QDialog dialog(&mainWindow);
    QWidget detached;
    QWidget tool(&mainWindow, Qt::Tool);
    QLineEdit text(&dialog);
    QSlider slider(&dialog);
    text.show();
    slider.show();

    ShortcutManager manager;
    int windowCalls = 0;
    int minimizeCalls = 0;
    int operatingCalls = 0;
    int keyingCalls = 0;
    QWidget* actedOn = nullptr;
    bool operatingAllowed = false;
    manager.registerAction("window_fullscreen", "Full screen", "Display",
        QKeySequence(Qt::Key_F11), [&] {
            ++windowCalls;
            actedOn = QApplication::activeWindow();
        }, false, false, ShortcutManager::ShortcutPolicy::WindowManagement);
    manager.registerAction("window_minimize", "Minimize", "Display",
        QKeySequence(Qt::Key_F10), [&] { ++minimizeCalls; }, false, false,
        ShortcutManager::ShortcutPolicy::WindowManagement);
    manager.registerAction("operating", "Operating", "Display",
        QKeySequence(Qt::Key_F8), [&] { ++operatingCalls; });
    // No transmitter or transport: this counter pins the policy refusal even
    // if a keying registration accidentally asks for the window exemption.
    manager.registerAction("keying_marker", "Keying marker", "TX",
        QKeySequence(Qt::Key_F9), [&] { ++keyingCalls; }, false, true,
        ShortcutManager::ShortcutPolicy::WindowManagement);
    const auto rebuild = [&] {
        manager.rebuildShortcuts(&mainWindow, [&] { return operatingAllowed; });
    };
    rebuild();

    focus(mainWindow);
    for (QWidget* window : {&mainWindow, static_cast<QWidget*>(&dialog), &detached, &tool}) {
        focus(*window);
        const int before = windowCalls;
        press(Qt::Key_F11);
        expect(windowCalls == before + 1 && actedOn == window,
               "window shortcut reaches the active top-level with operating guard closed");
        const int minimizeBefore = minimizeCalls;
        press(Qt::Key_F10);
        expect(minimizeCalls == minimizeBefore + 1, "second window action shares policy");
    }

    for (QWidget* input : {static_cast<QWidget*>(&text), static_cast<QWidget*>(&slider)}) {
        focus(dialog, input);
        manager.setShortcutsEnabled(false);
        const int before = windowCalls;
        press(Qt::Key_F11);
        expect(windowCalls == before + 1 && actedOn == &dialog,
               "window shortcut survives text/slider focus and operating disable");
    }

    focus(mainWindow);
    press(Qt::Key_F8);
    press(Qt::Key_F9);
    expect(operatingCalls == 0 && keyingCalls == 0, "disabled operating/keying actions stay silent");
    manager.setShortcutsEnabled(true);
    press(Qt::Key_F8);
    press(Qt::Key_F9);
    expect(operatingCalls == 0 && keyingCalls == 0, "operating guard still gates operating/keying actions");
    operatingAllowed = true;
    press(Qt::Key_F8);
    press(Qt::Key_F9);
    expect(operatingCalls == 1 && keyingCalls == 1, "operating actions retain normal enabled behavior");
    focus(detached);
    press(Qt::Key_F8);
    press(Qt::Key_F9);
    expect(operatingCalls == 1 && keyingCalls == 1, "operating/keying context stays window-scoped");

    manager.setBinding("window_fullscreen", QKeySequence(Qt::Key_F12));
    expect(manager.conflictCheck(QKeySequence(Qt::Key_F12)) == QStringLiteral("Full screen"),
           "window actions remain in configurable conflict checking");
    rebuild();
    rebuild();
    const int beforeRebind = windowCalls;
    press(Qt::Key_F11);
    expect(windowCalls == beforeRebind, "rebuild retires the old binding");
    press(Qt::Key_F12);
    expect(windowCalls == beforeRebind + 1, "rebuild keeps one application shortcut at the new binding");
    manager.clearBinding("window_fullscreen");
    rebuild();
    press(Qt::Key_F12);
    expect(windowCalls == beforeRebind + 1, "clearing a window binding removes its shortcut");
    return failures == 0 ? 0 : 1;
}
