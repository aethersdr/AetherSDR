// #6257 — StatusIndicator makes a click-handled status-bar label (or a small
// container of labels) operable without a mouse: Tab reaches it, Return/Enter/
// Space activate it, assistive technology sees a Button with a Press action,
// and a canon-cyan underline marks keyboard focus.
#include "TestSettingsProfile.h"
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
    press(dvk, Qt::Key_Space);
    report("Return, Enter and Space each activate it", dvkActivations == 3);
    press(dvk, Qt::Key_A);
    press(dvk, Qt::Key_Tab);
    report("other keys do not", dvkActivations == 3);

    press(stack, Qt::Key_Space);
    report("a container of labels activates the same way", stackActivations == 1);

    dvk->setEnabled(false);
    dvkHelper->activate();
    report("a disabled indicator does not activate", dvkActivations == 3);
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

    std::printf("\n%d/%d passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
