// #6257 — StatusIndicator makes a click-handled status-bar label (or a small
// container of labels) operable without a mouse: Tab reaches it, Return/Enter
// activate it, assistive technology sees a (checkable) Button with a Press
// action, and a canon-cyan underline marks keyboard focus. Space stays PTT.
#include "TestSettingsProfile.h"
#include "gui/PttHoldKeyStep.h"
#include "gui/StatusIndicator.h"

#include <QAccessible>
#include <QAccessibleActionInterface>
#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QVBoxLayout>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const char* label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}

void press(QWidget* widget, int key)
{
    QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(widget, &down);
}

int g_stateChanges = 0;
void countStateChanges(QAccessibleEvent* event)
{
    if (event->type() == QAccessible::StateChanged) {
        ++g_stateChanges;
    }
}

// MainWindow's app-level filter, reduced to its PTT (Hold) decision: with a
// radio connected and shortcuts on, pttHoldKeyStep() turns Space into a key.
struct PttFilter : QObject {
    bool holdActive{false};
    int pttKeys{0};
    bool eventFilter(QObject*, QEvent* e) override
    {
        if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease) {
            return false;
        }
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() != Qt::Key_Space || ke->isAutoRepeat()) {
            return false;
        }
        switch (pttHoldKeyStep(e->type(), holdActive, true, false, true)) {
        case PttHoldKeyStep::PassThrough:
            return false;
        case PttHoldKeyStep::KeyTx:
            holdActive = true;
            ++pttKeys;
            return true;
        case PttHoldKeyStep::UnkeyTx:
            holdActive = false;
            return true;
        case PttHoldKeyStep::Consume:
            return true;
        }
        return false;
    }
};

int g_objectCreated = 0;
int g_objectDestroyed = 0;
QAccessible::Role g_destroyedRole = QAccessible::NoRole;
bool g_destroyedBeforeCreated = true;
void countObjectCreated(QAccessibleEvent* event)
{
    if (event->type() == QAccessible::ObjectCreated) {
        ++g_objectCreated;
    } else if (event->type() == QAccessible::ObjectDestroyed) {
        if (g_objectCreated > g_objectDestroyed) {
            g_destroyedBeforeCreated = false;
        }
        ++g_objectDestroyed;
        if (QAccessibleInterface* iface = event->accessibleInterface()) {
            g_destroyedRole = iface->role();
        }
    }
}

QWidget* focusBar(QWidget* widget)
{
    return widget->findChild<QWidget*>(QStringLiteral("statusIndicatorFocusBar"));
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("status-indicator-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);

    QWidget bar;
    auto* layout = new QVBoxLayout(&bar);
    auto* dvk = new QLabel(QStringLiteral("DVK"));
    dvk->setAccessibleName(QStringLiteral("Digital Voice Keyer"));
    layout->addWidget(dvk);
    // A container like the TUN/AMP stacks: a widget holding two labels.
    auto* stack = new QWidget;
    auto* stackLayout = new QVBoxLayout(stack);
    stackLayout->addWidget(new QLabel(QStringLiteral("TUN")));
    stackLayout->addWidget(new QLabel(QStringLiteral("OPERATE")));
    stack->setAccessibleName(QStringLiteral("Tuner Genius XL status"));
    layout->addWidget(stack);
    bar.resize(200, 120);
    bar.show();

    StatusIndicator* dvkHelper = StatusIndicator::attach(dvk);
    StatusIndicator* stackHelper = StatusIndicator::attach(stack);
    int dvkActivations = 0;
    int stackActivations = 0;
    QObject::connect(dvkHelper, &StatusIndicator::activated, [&] { ++dvkActivations; });
    QObject::connect(stackHelper, &StatusIndicator::activated, [&] { ++stackActivations; });

    report("attach is idempotent and findable", StatusIndicator::attach(dvk) == dvkHelper
                                                    && StatusIndicator::of(dvk) == dvkHelper);
    report("the indicator is reachable by Tab", dvk->focusPolicy() == Qt::TabFocus);

    press(dvk, Qt::Key_Return);
    press(dvk, Qt::Key_Enter);
    press(dvk, Qt::Key_Return);
    report("Return and Enter each activate it", dvkActivations == 3);
    QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    space.ignore();
    QApplication::sendEvent(dvk, &space);
    report("Space does not activate it, and the helper does not take it",
           dvkActivations == 3 && !space.isAccepted());
    press(dvk, Qt::Key_A);
    press(dvk, Qt::Key_Tab);
    report("other keys do not", dvkActivations == 3);
    QKeyEvent held(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QString(), true);
    const bool consumed = QApplication::sendEvent(dvk, &held) && held.isAccepted();
    report("a held key's auto-repeat is consumed without acting again",
           dvkActivations == 3 && consumed);

    {
        // Review #6267: in the app, Space is PTT (Hold). Behind the same
        // app-level filter, a focused indicator keys PTT on Space and does not
        // activate; Return still activates it and keys nothing.
        PttFilter ptt;
        app.installEventFilter(&ptt);
        const int before = dvkActivations;
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(dvk, &down);
        QKeyEvent up(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(dvk, &up);
        report("connected: Space on a focused indicator is PTT, not the indicator",
               ptt.pttKeys == 1 && dvkActivations == before);
        press(dvk, Qt::Key_Return);
        report("connected: Return still activates it and keys nothing",
               dvkActivations == before + 1 && ptt.pttKeys == 1);
        app.removeEventFilter(&ptt);
        dvkActivations = before;
    }

    press(stack, Qt::Key_Return);
    report("a container of labels activates the same way", stackActivations == 1);

    dvk->setEnabled(false);
    press(dvk, Qt::Key_Return);
    dvkHelper->activate();
    report("a disabled indicator does not activate, by key or directly", dvkActivations == 3);
    dvk->setEnabled(true);

    QAccessible::setActive(true);
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(dvk);
    report("assistive technology sees a Button",
           iface && iface->role() == QAccessible::Button);
    report("named by its accessible name",
           iface && iface->text(QAccessible::Name) == QStringLiteral("Digital Voice Keyer"));
    QAccessibleActionInterface* actions = iface ? iface->actionInterface() : nullptr;
    report("with a Press action",
           actions && actions->actionNames().contains(QAccessibleActionInterface::pressAction()));
    if (actions) {
        actions->doAction(QAccessibleActionInterface::pressAction());
    }
    report("the Press action activates it", dvkActivations == 4);
    QAccessibleInterface* stackIface = QAccessible::queryAccessibleInterface(stack);
    report("a container is a Button too", stackIface && stackIface->role() == QAccessible::Button);
    QAccessibleInterface* plain = QAccessible::queryAccessibleInterface(stack->findChild<QLabel*>());
    report("labels inside a container stay plain text",
           plain && plain->role() != QAccessible::Button);

    {
        // On/off indicators report checked state; colour alone is not a state.
        QAccessible::installUpdateHandler(countStateChanges);
        dvkHelper->setCheckable(true);
        g_stateChanges = 0;
        report("a checkable indicator starts unchecked",
               iface && iface->state().checkable && !iface->state().checked);
        dvkHelper->setChecked(true);
        report("checking it reports Checked and one state change",
               iface && iface->state().checked && g_stateChanges == 1);
        dvkHelper->setChecked(true);
        report("setting the same state again announces nothing", g_stateChanges == 1);
        StatusIndicator::setCheckedFor(dvk, false);
        report("setCheckedFor reaches the helper",
               iface && !iface->state().checked && g_stateChanges == 2);
        StatusIndicator::setCheckedFor(new QLabel(&bar), true);  // no helper: no-op
        report("a non-toggle indicator is not checkable",
               stackIface && !stackIface->state().checkable);
        QAccessible::installUpdateHandler(nullptr);
    }

    QWidget* underline = focusBar(dvk);
    report("the focus mark exists and starts hidden", underline && !underline->isVisible());
    QFocusEvent in(QEvent::FocusIn, Qt::TabFocusReason);
    QApplication::sendEvent(dvk, &in);
    report("keyboard focus shows the underline along the bottom edge",
           underline && underline->isVisible() && underline->height() == 2
               && underline->width() == dvk->width()
               && underline->geometry().bottom() == dvk->height() - 1);
    QFocusEvent out(QEvent::FocusOut, Qt::TabFocusReason);
    QApplication::sendEvent(dvk, &out);
    report("losing focus hides it", underline && !underline->isVisible());
    report("the underline never takes focus or clicks",
           underline && underline->focusPolicy() == Qt::NoFocus
               && underline->testAttribute(Qt::WA_TransparentForMouseEvents));

    // Review #6267: with accessibility already active when the indicator is
    // built and named (as MainWindow does), Qt caches the plain interface
    // before attach(). The helper must replace it with the Button.
    {
        QAccessible::installUpdateHandler(countObjectCreated);
        auto* late = new QLabel(QStringLiteral("FDX"));
        layout->addWidget(late);
        late->setAccessibleName(QStringLiteral("Full duplex"));
        QAccessibleInterface* before = QAccessible::queryAccessibleInterface(late);
        report("active accessibility: the named label starts as plain text",
               before && before->role() != QAccessible::Button);
        auto* lateStack = new QWidget;
        lateStack->setAccessibleName(QStringLiteral("Power Genius XL status"));
        layout->addWidget(lateStack);
        QAccessible::queryAccessibleInterface(lateStack);

        g_objectCreated = 0;
        g_objectDestroyed = 0;
        StatusIndicator* lateHelper = StatusIndicator::attach(late);
        StatusIndicator::attach(lateStack);
        int lateActivations = 0;
        QObject::connect(lateHelper, &StatusIndicator::activated, [&] { ++lateActivations; });
        QAccessibleInterface* after = QAccessible::queryAccessibleInterface(late);
        QAccessibleActionInterface* lateActions = after ? after->actionInterface() : nullptr;
        report("attach replaces the cached interface with a Button",
               after && after->role() == QAccessible::Button);
        report("the replacement keeps the accessible name",
               after && after->text(QAccessible::Name) == QStringLiteral("Full duplex"));
        report("and offers Press, which activates it",
               lateActions
                   && lateActions->actionNames().contains(QAccessibleActionInterface::pressAction()));
        if (lateActions) {
            lateActions->doAction(QAccessibleActionInterface::pressAction());
        }
        report("Press on the replacement activates it", lateActivations == 1);
        QAccessibleInterface* stackAfter = QAccessible::queryAccessibleInterface(lateStack);
        report("a cached container is replaced with a Button too",
               stackAfter && stackAfter->role() == QAccessible::Button);
        report("each replacement tells assistive clients about the new object",
               g_objectCreated == 2);
        report("and first retires the old object it replaces",
               g_objectDestroyed == 2 && g_destroyedBeforeCreated
                   && g_destroyedRole != QAccessible::Button);
        QAccessible::installUpdateHandler(nullptr);
    }

    std::printf("\n%d/%d passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
