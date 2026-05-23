// Phase 1 smoke tests for the container system — exercises the
// ContainerWidget <-> FloatingContainerWindow float/dock cycle.
//
// Headless-ish: uses QApplication so the widgets can instantiate and
// process signals, but never calls show() (no X11 / display needed).
// Run:   ./build/container_widget_test

#include "gui/containers/ContainerTitleBar.h"
#include "gui/containers/ContainerWidget.h"
#include "gui/containers/FloatingContainerWindow.h"

#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>

using namespace MasterSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-56s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name, detail.c_str());
    if (!ok) ++g_failed;
}

void testContainerBasics()
{
    ContainerWidget c("test_id", "Test Container");

    report("id stored",     c.id() == "test_id",
           c.id().toStdString());
    report("title stored",  c.title() == "Test Container",
           c.title().toStdString());
    report("default dock mode is PanelDocked",
           c.dockMode() == ContainerWidget::DockMode::PanelDocked);
    report("default visible",
           c.isContainerVisible() && !c.isFloating());
}

void testSetContent()
{
    ContainerWidget c("id", "T");
    auto* l1 = new QLabel("one");
    auto* l2 = new QLabel("two");

    QWidget* prev = c.setContent(l1);
    report("first setContent returns null prev",
           prev == nullptr);
    report("first setContent sets content",
           c.content() == l1);

    prev = c.setContent(l2);
    report("second setContent returns prior content",
           prev == l1,
           QString::asprintf("got=%p want=%p", prev, l1).toStdString());
    report("second setContent replaces content",
           c.content() == l2);

    // Cleanup — the previous content is detached, caller owns it now.
    delete l1;
    // l2 still parented to container; destructor cleans up.
}

void testVisibilitySignal()
{
    ContainerWidget c("id", "T");
    QSignalSpy spy(&c, &ContainerWidget::visibilityChanged);

    c.setContainerVisible(false);
    report("visibility change emits signal",
           spy.count() == 1 && spy.takeFirst().value(0).toBool() == false);
    report("isContainerVisible reflects false",
           !c.isContainerVisible());

    c.setContainerVisible(true);
    report("visibility change back emits again",
           spy.count() == 1 && spy.takeFirst().value(0).toBool() == true);

    // No-op should not re-emit.
    c.setContainerVisible(true);
    report("no-op setVisible suppresses signal",
           spy.count() == 0);
}

void testFloatDockCycle()
{
    ContainerWidget c("id", "T");
    auto* body = new QLabel("payload");
    c.setContent(body);

    QSignalSpy floatSpy(&c, &ContainerWidget::floatRequested);
    QSignalSpy dockSpy (&c, &ContainerWidget::dockRequested);
    QSignalSpy modeSpy (&c, &ContainerWidget::dockModeChanged);

    // Simulate clicking the float button.  Titlebar emits
    // floatToggleClicked → ContainerWidget emits floatRequested.
    emit c.titleBar()->floatToggleClicked();
    report("floatRequested emitted on titlebar toggle",
           floatSpy.count() == 1 && dockSpy.count() == 0);

    // Manager would normally move the container into a floating
    // window; emulate that directly.
    FloatingContainerWindow win;
    win.takeContainer(&c);
    report("takeContainer transitions to Floating",
           c.isFloating() && win.container() == &c);
    report("dockMode change signal fired",
           modeSpy.count() >= 1);

    // Release → back to docked state.
    ContainerWidget* released = win.releaseContainer();
    report("releaseContainer returns the same container",
           released == &c);
    report("released container is back in PanelDocked mode",
           c.isPanelDocked());
    report("window no longer has a container",
           win.container() == nullptr);

    // When floating, the toggle should emit dockRequested instead.
    FloatingContainerWindow win2;
    win2.takeContainer(&c);
    floatSpy.clear();
    dockSpy.clear();
    emit c.titleBar()->floatToggleClicked();
    report("toggle while floating emits dockRequested",
           dockSpy.count() == 1 && floatSpy.count() == 0);

    // Cleanup.
    win2.releaseContainer();
}

void testCloseSignal()
{
    ContainerWidget c("id", "T");
    QSignalSpy spy(&c, &ContainerWidget::closeRequested);
    emit c.titleBar()->closeClicked();
    report("closeRequested emitted on titlebar close",
           spy.count() == 1);
}

void testTitlebarCloseButtonToggle()
{
    ContainerWidget c("id", "T");
    auto* tb = c.titleBar();
    // Just verifying the API doesn't crash — visual effect needs
    // manual inspection.
    tb->setCloseButtonVisible(false);
    tb->setCloseButtonVisible(true);
    report("setCloseButtonVisible toggles cleanly", true);
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    std::printf("Container system Phase 1 test harness\n\n");

    testContainerBasics();
    testSetContent();
    testVisibilitySignal();
    testFloatDockCycle();
    testCloseSignal();
    testTitlebarCloseButtonToggle();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
