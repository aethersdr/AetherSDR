#include "TestSettingsProfile.h"
#include "core/ShortcutManager.h"
#include "gui/TxKeyActivationGuard.h"

#include <QAccessible>
#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
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

    // #5483: "keyboard shortcuts off" must mean the bound key falls through to
    // the focused widget, not that it disappears. A QShortcut that is enabled
    // but refused by its guard still consumes the key; only a disabled one
    // lets it through. MainWindow disables the operating shortcuts when the
    // master switch is off, and the state has to survive a rebuild, because
    // Configure Shortcuts rebuilds after the switch was read.
    {
        QWidget host;
        QPushButton button(QStringLiteral("focused"), &host);
        button.setFocusPolicy(Qt::StrongFocus);
        button.show();
        ShortcutManager spaceManager;
        int spaceCalls = 0;
        int clicks = 0;
        bool spaceAllowed = false;
        QObject::connect(&button, &QPushButton::clicked, [&] { ++clicks; });
        spaceManager.registerAction("space_marker", "Space marker", "TX",
            QKeySequence(Qt::Key_Space), [&] { ++spaceCalls; });
        const auto rebuildSpace = [&] {
            spaceManager.rebuildShortcuts(&host, [&] { return spaceAllowed; });
        };

        spaceManager.setShortcutsEnabled(false);
        rebuildSpace();
        focus(host, &button);
        press(Qt::Key_Space);
        expect(clicks == 1 && spaceCalls == 0,
               "disabled shortcut lets its key reach the focused button, across a rebuild");
        expect(!spaceManager.shortcutsEnabled(), "rebuild keeps the disabled state");

        spaceManager.setShortcutsEnabled(true);
        press(Qt::Key_Space);
        expect(clicks == 1 && spaceCalls == 0,
               "an enabled shortcut refused by its guard still swallows the key");

        spaceAllowed = true;
        press(Qt::Key_Space);
        expect(clicks == 1 && spaceCalls == 1,
               "an enabled, allowed shortcut acts and keeps the key from the button");
    }

    // #5483 follow-up: the key that now reaches the focused widget must not
    // KEY THE TRANSMITTER. Fusion gives a clicked QPushButton focus, so after a
    // mouse click on MOX the next Space landed on MOX and latched TX. The
    // route below is MainWindow::eventFilter()'s order -- PTT (Hold) first,
    // consuming its key only while shortcuts are on, then the shared guard
    // refuseTxKeyActivation() that MainWindow calls. No radio is constructed.
    {
        struct KeyRoute : QObject {
            ShortcutManager* shortcuts{nullptr};
            bool shortcutsOn{false};
            int pttPresses{0};
            int pttReleases{0};
            int refused{0};
            bool eventFilter(QObject* obj, QEvent* e) override
            {
                if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease)
                    return false;
                auto* ke = static_cast<QKeyEvent*>(e);
                const auto* a = shortcuts->actionForKey(QKeySequence(ke->key()));
                if (shortcutsOn && !ke->isAutoRepeat() && a
                    && a->id == QLatin1String("ptt_hold")) {
                    ++(e->type() == QEvent::KeyPress ? pttPresses : pttReleases);
                    return true;
                }
                if (AetherSDR::refuseTxKeyActivation(obj, ke, shortcutsOn, *shortcuts)) {
                    ++refused;
                    return true;
                }
                return false;
            }
        };

        QDialog host;
        QPushButton mox(QStringLiteral("MOX"), &host);
        mox.setCheckable(true);
        AetherSDR::markTxKeying(&mox);
        QPushButton ordinary(QStringLiteral("ordinary"), &host);
        QPushButton receiveOnly(QStringLiteral("receive"), &host);
        AetherSDR::registerReceiveControlAction(&receiveOnly,
            [](const std::shared_ptr<AetherSDR::TxController>&, const QString&,
               const QString&) -> AetherSDR::TxKeyingAction::Prepared { return {}; });
        for (QPushButton* b : {&mox, &ordinary, &receiveOnly}) {
            b->setFocusPolicy(Qt::StrongFocus);
            b->setAutoDefault(false);
            b->show();
        }
        int moxClicks = 0;
        int ordinaryClicks = 0;
        int receiveClicks = 0;
        QObject::connect(&mox, &QPushButton::clicked, [&] { ++moxClicks; });
        QObject::connect(&ordinary, &QPushButton::clicked, [&] { ++ordinaryClicks; });
        QObject::connect(&receiveOnly, &QPushButton::clicked, [&] { ++receiveClicks; });

        ShortcutManager keys;
        // As MainWindow registers it: null handler (no QShortcut), keysTx.
        keys.registerAction("ptt_hold", "PTT (Hold)", "TX",
            QKeySequence(Qt::Key_Space), nullptr, false, true);
        KeyRoute route;
        route.shortcuts = &keys;
        app.installEventFilter(&route);

        route.shortcutsOn = false;
        focus(host, &mox);
        press(Qt::Key_Space);
        expect(moxClicks == 0 && !mox.isChecked() && route.refused == 2,
               "shortcuts off: bound Space on a focused TX-keying button does not key it");

        focus(host, &ordinary);
        press(Qt::Key_Space);
        expect(ordinaryClicks == 1,
               "shortcuts off: bound Space still activates an ordinary focused button");

        focus(host, &receiveOnly);
        press(Qt::Key_Space);
        expect(receiveClicks == 1,
               "shortcuts off: a receive-only control marked for the bridge still activates");

        // Not widened: an activation key bound to nothing is untouched, as at
        // origin/main -- Return on an auto-default TX button in a dialog.
        mox.setAutoDefault(true);
        focus(host, &mox);
        press(Qt::Key_Return);
        expect(moxClicks == 1 && mox.isChecked(),
               "shortcuts off: an UNBOUND activation key on a TX button is unchanged");
        mox.setAutoDefault(false);
        mox.setChecked(false);

        // The screen-reader press is not a key event and stays available.
        QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(&mox);
        QAccessibleActionInterface* actions = iface ? iface->actionInterface() : nullptr;
        expect(actions != nullptr, "TX button exposes an accessible action interface");
        if (actions) {
            actions->doAction(QAccessibleActionInterface::pressAction());
            QTest::qWait(400);  // allow for an animateClick()-style press
        }
        expect(moxClicks == 2,
               "shortcuts off: the accessible press action still activates the TX button");
        mox.setChecked(false);

        // Shortcuts on: PTT (Hold) owns Space, press and release, before the
        // guard; the guard itself never fires.
        route.shortcutsOn = true;
        const int refusedBefore = route.refused;
        focus(host, &mox);
        press(Qt::Key_Space);
        expect(route.pttPresses == 1 && route.pttReleases == 1 && moxClicks == 2
                   && route.refused == refusedBefore,
               "shortcuts on: Space goes to PTT (Hold), not to the focused button");
        QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        expect(!AetherSDR::refuseTxKeyActivation(&mox, &space, true, keys),
               "shortcuts on: the guard never refuses");

        app.removeEventFilter(&route);
    }
    return failures == 0 ? 0 : 1;
}
