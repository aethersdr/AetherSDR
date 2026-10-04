#include "TestSettingsProfile.h"
#include "core/ShortcutManager.h"
#include "gui/PttHoldKeyStep.h"
#include "gui/ShortcutRefusalNotice.h"
#include "gui/StatusBarNotice.h"
#include "gui/TxKeyActivationGuard.h"

#include <QAccessible>
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QStatusBar>
#include <QTest>
#include <QVBoxLayout>
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

// Stands in for the main window: counts the presses no child accepted.
class UnacceptedKeyProbe : public QWidget {
public:
    int presses{0};

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        ++presses;
        QWidget::keyPressEvent(event);
    }
};

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

    // PTT (Hold) decision, MainWindow::handlePttHoldShortcut()'s switch.
    // Principle VI: the release of a hold that STARTED keyed always un-keys,
    // whatever changed mid-hold -- shortcuts switched off, focus moved into a
    // text field, the connection dropped. Only a new press is gated.
    {
        using AetherSDR::PttHoldKeyStep;
        using AetherSDR::pttHoldKeyStep;
        const auto rel = QEvent::KeyRelease;
        const auto prs = QEvent::KeyPress;
        //                   type, holdActive, shortcutsOn, typing, connected
        expect(pttHoldKeyStep(rel, true, false, false, true) == PttHoldKeyStep::UnkeyTx,
               "PTT hold: shortcuts turned OFF mid-hold, the release still un-keys");
        expect(pttHoldKeyStep(rel, true, true, true, true) == PttHoldKeyStep::UnkeyTx,
               "PTT hold: focus moved into a text field mid-hold, the release still un-keys");
        expect(pttHoldKeyStep(rel, true, true, false, false) == PttHoldKeyStep::UnkeyTx,
               "PTT hold: disconnected mid-hold, the release still un-keys");
        expect(pttHoldKeyStep(rel, true, false, true, false) == PttHoldKeyStep::UnkeyTx,
               "PTT hold: every gate closed mid-hold, the release still un-keys");
        expect(pttHoldKeyStep(rel, true, true, false, true) == PttHoldKeyStep::UnkeyTx,
               "PTT hold: normal release un-keys");
        expect(pttHoldKeyStep(prs, false, true, false, true) == PttHoldKeyStep::KeyTx,
               "PTT hold: press keys with shortcuts on, connected, not typing");
        expect(pttHoldKeyStep(prs, false, false, false, true) == PttHoldKeyStep::PassThrough,
               "PTT hold: press with shortcuts off does not key and falls through (#5483)");
        expect(pttHoldKeyStep(prs, false, true, true, true) == PttHoldKeyStep::PassThrough,
               "PTT hold: press while typing does not key and falls through");
        expect(pttHoldKeyStep(prs, false, true, false, false) == PttHoldKeyStep::PassThrough,
               "PTT hold: press while disconnected does not key and falls through");
        expect(pttHoldKeyStep(prs, true, true, false, true) == PttHoldKeyStep::Consume,
               "PTT hold: a second press during a hold is consumed, not re-keyed");
        expect(pttHoldKeyStep(rel, false, true, false, true) == PttHoldKeyStep::Consume,
               "PTT hold: a stray release with no hold is consumed, nothing to un-key");
        expect(pttHoldKeyStep(rel, false, false, false, true) == PttHoldKeyStep::PassThrough,
               "PTT hold: a stray release with shortcuts off falls through");
    }

    // #5483 follow-up: the key that now reaches the focused widget must not
    // KEY THE TRANSMITTER. Fusion gives a clicked QPushButton focus, so after a
    // mouse click on MOX the next Space landed on MOX and latched TX. The
    // route below is MainWindow::eventFilter()'s order -- PTT (Hold) first,
    // through the same pttHoldKeyStep() MainWindow uses, then the shared guard
    // refuseTxKeyActivation() that MainWindow calls. No radio is constructed.
    //
    // Coverage boundary, stated: no registered test target links
    // MainWindow*.cpp, so this route RE-IMPLEMENTS eventFilter()'s ordering.
    // Nothing here pins that the production filter calls
    // refuseTxKeyActivation() at all, or that it sits after
    // handlePttHoldShortcut(). It resolves bindings with the same
    // shortcutSequenceFromKeyEvent() production uses, and passes "connected,
    // not typing" as constants where production reads textEntryCaptured() and
    // RadioModel::isConnected(); the decision table above covers those gates.
    {
        struct KeyRoute : QObject {
            ShortcutManager* shortcuts{nullptr};
            bool shortcutsOn{false};
            bool holdActive{false};
            int pttPresses{0};
            int pttReleases{0};
            int refused{0};
            bool eventFilter(QObject* obj, QEvent* e) override
            {
                if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease)
                    return false;
                auto* ke = static_cast<QKeyEvent*>(e);
                const auto* a = shortcuts->actionForKey(
                    AetherSDR::shortcutSequenceFromKeyEvent(ke));
                if (!ke->isAutoRepeat() && a && a->id == QLatin1String("ptt_hold")) {
                    switch (AetherSDR::pttHoldKeyStep(e->type(), holdActive, shortcutsOn,
                                                      false, true)) {
                    case AetherSDR::PttHoldKeyStep::PassThrough:
                        break;
                    case AetherSDR::PttHoldKeyStep::Consume:
                        return true;
                    case AetherSDR::PttHoldKeyStep::KeyTx:
                        holdActive = true;
                        ++pttPresses;
                        return true;
                    case AetherSDR::PttHoldKeyStep::UnkeyTx:
                        holdActive = false;
                        ++pttReleases;
                        return true;
                    }
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

        // Principle VI through real key delivery: key with Space, switch
        // shortcuts OFF while it is held, release. The release must un-key and
        // be consumed -- it must not stay keyed, and must not reach MOX.
        focus(host, &ordinary);
        QTest::keyPress(&ordinary, Qt::Key_Space);
        expect(route.holdActive && route.pttPresses == 2,
               "mid-hold toggle: Space press keyed with shortcuts on");
        route.shortcutsOn = false;
        QTest::keyRelease(&ordinary, Qt::Key_Space);
        expect(!route.holdActive && route.pttReleases == 2 && ordinaryClicks == 1,
               "mid-hold toggle: shortcuts turned off mid-hold, the release still un-keys");

        // The guard shares the bridge's predicate: an UNMARKED button whose
        // name reads as a TX keyer (the bridge's name fallback) is refused too.
        QPushButton unmarked(QStringLiteral("PTT"), &host);
        unmarked.setFocusPolicy(Qt::StrongFocus);
        unmarked.setAutoDefault(false);
        unmarked.show();
        int unmarkedClicks = 0;
        QObject::connect(&unmarked, &QPushButton::clicked, [&] { ++unmarkedClicks; });
        focus(host, &unmarked);
        press(Qt::Key_Space);
        expect(unmarkedClicks == 0,
               "shortcuts off: an unmarked button named like a TX keyer is refused (bridge fallback)");

        // ... and a button inside a marked TX control is judged by its chain.
        QWidget keyingPanel(&host);
        AetherSDR::markTxKeying(&keyingPanel);
        QPushButton inner(QStringLiteral("inner"), &keyingPanel);
        inner.setFocusPolicy(Qt::StrongFocus);
        inner.setAutoDefault(false);
        keyingPanel.show();
        inner.show();
        int innerClicks = 0;
        QObject::connect(&inner, &QPushButton::clicked, [&] { ++innerClicks; });
        focus(host, &inner);
        press(Qt::Key_Space);
        expect(innerClicks == 0,
               "shortcuts off: a button inside a marked TX control is refused (parent chain)");

        app.removeEventFilter(&route);
    }

    // #5483 item 2: with shortcuts off, the first refused bound key in a
    // session is announced once. Window-management keys are not refused.
    {
        ShortcutManager refusal;
        refusal.registerAction("mox_toggle", "MOX Toggle", "TX",
            QKeySequence(Qt::Key_T), [] {}, false, true);
        refusal.registerAction("window_fullscreen", "Full screen", "Display",
            QKeySequence(Qt::Key_F11), [] {}, false, false,
            ShortcutManager::ShortcutPolicy::WindowManagement);
        refusal.registerAction("keying_wm", "Keying asks for WM", "TX",
            QKeySequence(Qt::Key_F9), [] {}, false, true,
            ShortcutManager::ShortcutPolicy::WindowManagement);
        expect(refusal.operatingActionForKey(QKeySequence(Qt::Key_T)) != nullptr,
               "refusal: an operating binding resolves");
        expect(refusal.operatingActionForKey(QKeySequence(Qt::Key_F11)) == nullptr,
               "refusal: a window-management binding is not governed by the switch");
        expect(refusal.operatingActionForKey(QKeySequence(Qt::Key_F9)) != nullptr,
               "refusal: a keying action asking for window management stays operating");
        expect(refusal.operatingActionForKey(QKeySequence(Qt::Key_Q)) == nullptr,
               "refusal: an unbound key resolves to nothing");

        const QKeyEvent tPress(QEvent::KeyPress, Qt::Key_T, Qt::NoModifier);
        const QKeyEvent tRepeat(QEvent::KeyPress, Qt::Key_T, Qt::NoModifier,
                                QString(), /*autorep=*/true);
        const QKeyEvent tRelease(QEvent::KeyRelease, Qt::Key_T, Qt::NoModifier);
        const ShortcutManager::Action* bound = refusal.operatingActionForKey(
            AetherSDR::shortcutSequenceFromKeyEvent(&tPress));

        AetherSDR::ShortcutRefusalNotice notice;
        const ShortcutManager::Action* unbound = nullptr;
        // take(event, shortcutsEnabled, inputCaptured, noticeVisible, action)
        expect(!notice.take(&tPress, /*shortcutsEnabled=*/true, false, true, bound),
               "refusal: no notice while shortcuts are on");
        expect(!notice.take(&tPress, false, /*inputCaptured=*/true, true, bound),
               "refusal: no notice while a text field or slider holds the keys");
        expect(!notice.take(&tPress, false, false, true, unbound),
               "refusal: no notice for an unbound key");
        expect(!notice.take(&tRelease, false, false, true, bound),
               "refusal: no notice on a key release");
        expect(!notice.take(&tRepeat, false, false, true, bound),
               "refusal: no notice on auto-repeat");
        expect(!notice.take(&tPress, false, false, /*noticeVisible=*/false, bound)
                   && !notice.given(),
               "refusal: a notice that cannot be shown is not spent");
        expect(!notice.given(), "refusal: nothing given before a refused press");
        const ShortcutManager::Action* refused =
            notice.take(&tPress, false, false, true, bound);
        expect(bound && refused == bound
                   && refused->displayName == QLatin1String("MOX Toggle"),
               "refusal: the first bound press hands back the action it refused");
        expect(notice.given() && !notice.take(&tPress, false, false, true, bound),
               "refusal: the notice is given once per session");

        // The capture test follows the action: a hold key yields to text entry
        // only, as its handler does; a QShortcut action to every capture.
        using AetherSDR::shortcutRefusalInputCaptured;
        expect(!shortcutRefusalInputCaptured(/*holdKeyAction=*/true,
                   /*textEntryCaptured=*/false, /*shortcutInputCaptured=*/true),
               "refusal: a hold key is noticed over a combo or a slider lease");
        expect(shortcutRefusalInputCaptured(true, true, true),
               "refusal: a hold key typed into a text field is not noticed");
        expect(shortcutRefusalInputCaptured(false, false, true),
               "refusal: a QShortcut key under a combo or lease is not noticed");
        expect(!shortcutRefusalInputCaptured(false, false, false),
               "refusal: a QShortcut key with nothing capturing is noticed");
    }

    // The notice is raised from the window's keyPressEvent(), which Qt calls
    // only for a key no child accepted. These pin that premise on this Qt.
    {
        UnacceptedKeyProbe probe;
        auto* layout = new QVBoxLayout(&probe);
        auto* button = new QPushButton(QStringLiteral("RX control"));
        auto* plain = new QWidget;
        plain->setFocusPolicy(Qt::StrongFocus);
        auto* scroll = new QScrollArea;
        auto* scrolled = new QWidget;
        scrolled->setFocusPolicy(Qt::StrongFocus);
        scroll->setWidget(scrolled);
        layout->addWidget(button);
        layout->addWidget(plain);
        layout->addWidget(scroll);
        QDialog other(&probe);
        QPushButton otherButton(&other);

        focus(probe, button);
        press(Qt::Key_Space);
        expect(probe.presses == 0,
               "refusal: Space a focused button accepts does not reach the window");
        press(Qt::Key_T);
        expect(probe.presses == 1,
               "refusal: a key the focused button ignores reaches the window once");
        focus(probe, plain);
        press(Qt::Key_Space);
        press(Qt::Key_Left);
        expect(probe.presses == 3,
               "refusal: Space and an arrow nothing accepts reach the window");
        focus(probe, scrolled);
        press(Qt::Key_Left);
        expect(probe.presses == 3,
               "refusal: an arrow inside a scroll area is taken by the scroll area");
        press(Qt::Key_Space);
        expect(probe.presses == 4,
               "refusal: Space inside a scroll area reaches the window");
        focus(other, &otherButton);
        press(Qt::Key_T);
        expect(probe.presses == 4,
               "refusal: a key in another top-level window does not reach the window");

        // The TX key guard consumes its key; its site checks the window itself.
        using AetherSDR::shortcutRefusalReceiverInWindow;
        expect(shortcutRefusalReceiverInWindow(button, &probe),
               "refusal: a guarded button in the window is noticed");
        expect(!shortcutRefusalReceiverInWindow(&otherButton, &probe),
               "refusal: a guarded button in another top-level window is not noticed");
        expect(!shortcutRefusalReceiverInWindow(nullptr, &probe)
                   && !shortcutRefusalReceiverInWindow(button, nullptr),
               "refusal: no receiver or no window is not noticed");
    }

    // The notice sits in a permanent label, so the status bar's other widgets
    // stay on screen; showMessage() hides them, which the control shows.
    {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* bar = new QStatusBar;
        bar->setSizeGripEnabled(false);
        auto* indicator = new QLabel(QStringLiteral("TX"));
        bar->addWidget(indicator, 1);
        layout->addStretch(1);
        layout->addWidget(bar);
        host.resize(900, 120);
        focus(host);
        const int barHeight = bar->height();
        const int hostMinWidth = host.minimumSizeHint().width();

        AetherSDR::StatusBarNoticeLabel notice(bar);
        QApplication::processEvents();
        expect(!notice.isVisible() && indicator->isVisible(),
               "notice: the label is hidden until a notice is shown");
        expect(host.minimumSizeHint().width() <= hostMinWidth + 16,
               "notice: the hidden label does not widen the window");

        const QString text = QStringLiteral("Keyboard shortcuts are off.");
        notice.showNotice(text, 150);
        QApplication::processEvents();
        expect(notice.isVisible() && notice.text() == text,
               "notice: the label shows the text");
        expect(notice.accessibleName() == text,
               "notice: the label's accessible name is the same text");
        expect(indicator->isVisible() && bar->currentMessage().isEmpty(),
               "notice: the status bar's other widgets stay visible");
        expect(bar->height() == barHeight,
               "notice: the status bar keeps its height");
        expect(host.minimumSizeHint().width() <= hostMinWidth + 16,
               "notice: the shown label does not widen the window");
        expect(notice.geometry().right() <= bar->width()
                   && notice.geometry().left() >= indicator->geometry().right(),
               "notice: the label sits beside the indicator, inside the bar");
        QTest::qWait(400);
        expect(!notice.isVisible() && notice.text().isEmpty()
                   && notice.accessibleName().isEmpty(),
               "notice: the label hides and clears when its time is up");

        // A notice wider than the bar is clipped; it never resizes the window.
        host.resize(hostMinWidth + 40, 120);
        QApplication::processEvents();
        const int narrowWidth = host.width();
        notice.showNotice(text.repeated(8), 150);
        QApplication::processEvents();
        expect(host.width() == narrowWidth && indicator->isVisible()
                   && indicator->width() >= indicator->minimumSizeHint().width(),
               "notice: a notice wider than the bar leaves the window and indicator alone");
        notice.clearNotice();

        // Positive control: the same bar, with showMessage(), loses the indicator.
        bar->showMessage(text, 150);
        QApplication::processEvents();
        expect(!indicator->isVisible(),
               "notice control: showMessage() hides the indicator");
        bar->clearMessage();
        QApplication::processEvents();
        expect(indicator->isVisible(), "notice control: the indicator returns");
        bar->removeWidget(&notice);
    }
    return failures == 0 ? 0 : 1;
}
