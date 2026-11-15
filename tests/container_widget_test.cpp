// Phase 1 smoke tests for the container system — exercises the
// ContainerWidget <-> FloatingContainerWindow float/dock cycle.
//
// Uses QApplication and the offscreen platform to exercise real show/hide
// and reparent behavior without a radio, sound device or visible desktop.
// Run:   ./build/container_widget_test

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/containers/ContainerTitleBar.h"
#include "gui/containers/ContainerWidget.h"
#include "gui/containers/FloatingContainerWindow.h"

#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>

using namespace AetherSDR;

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

void testTransientPresentation()
{
    QWidget panel;
    auto* layout = new QVBoxLayout(&panel);
    ContainerWidget c("conditional", "Conditional");
    c.setContent(new QLabel("payload"));
    layout->addWidget(&c);
    QSignalSpy visibility(&c, &ContainerWidget::visibilityChanged);

    // makeEntry's default-open call is a logical no-op on a fresh container.
    // The first availability transition must still retain that open intent.
    c.setContainerVisible(true);
    c.setPresentationAvailable(false);
    panel.show();
    c.setPresentationAvailable(true);
    report("default logical open survives the first construction gate", c.isVisible());
    c.setPresentationAvailable(false);
    QWidget* generic = &c;
    generic->show();
    report("direct show cannot expose a suppressed docked container",
           c.isContainerVisible() && c.isHidden() && !c.isVisible());
    panel.hide();
    panel.show();
    report("ancestor show cannot bypass presentation suppression", c.isHidden());
    c.setPresentationAvailable(true);
    report("availability restores the requested docked presentation", c.isVisible());
    panel.hide();
    c.setPresentationAvailable(false);
    c.setPresentationAvailable(true);
    panel.show();
    report("ancestor hiding does not become an explicit child hide", c.isVisible());
    report("availability emits no logical close or open", visibility.isEmpty());

    c.setPresentationAvailable(false);
    generic->hide();
    c.setPresentationAvailable(true);
    report("explicit direct hide remains hidden after availability returns",
           c.isHidden() && c.isContainerVisible());
    c.setContainerVisible(false);
    c.setPresentationAvailable(false);
    c.setContainerVisible(true);
    report("logical reopen under suppression stays physically hidden",
           c.isContainerVisible() && c.isHidden());
    c.setPresentationAvailable(true);
    report("logical reopen restores when available", c.isVisible());
    c.setContainerVisible(false);
    c.setPresentationAvailable(false);
    c.setPresentationAvailable(true);
    report("logical close remains closed across availability changes",
           !c.isContainerVisible() && c.isHidden());
}

void testFloatingPresentation()
{
    ContainerWidget c("conditional-float", "Conditional float");
    c.setContent(new QLabel("payload"));
    QSignalSpy visibility(&c, &ContainerWidget::visibilityChanged);
    FloatingContainerWindow win;
    c.setPresentationAvailable(false);
    win.takeContainer(&c);
    QWidget* generic = &win;
    generic->show();
    report("suppressed float hides its outer window but keeps content shown",
           win.isHidden() && !c.isHidden() && c.isContainerVisible());
    c.setPresentationAvailable(true);
    report("float availability returns without blank content",
           win.isVisible() && c.isVisible() && c.content()->isVisible());
    c.setPresentationAvailable(false);
    win.setAlwaysOnTop(true);
    win.setFramelessMode(false);
    report("window flag changes cannot reveal a suppressed float", win.isHidden());
    c.setPresentationAvailable(true);
    report("window flag recreation retains the suppressed show request",
           win.isVisible() && c.content()->isVisible());
    c.setPresentationAvailable(false);
    generic->hide();
    c.setPresentationAvailable(true);
    report("explicitly hidden float is not reopened by availability", win.isHidden());
    generic->show();
    c.setPresentationAvailable(false);
    c.setContainerVisible(false);
    c.setPresentationAvailable(true);
    report("logical close during suppressed float cannot restore a blank window",
           win.isHidden() && !c.isContainerVisible());
    generic->show();
    report("unconditional restore show cannot override a managed logical close", win.isHidden());
    c.setContainerVisible(true);
    report("logical reopen restores both managed window and content",
           win.isVisible() && c.isVisible() && c.content()->isVisible());
    c.setContainerVisible(false);
    report("logical close also suppresses an available managed float", win.isHidden());
    c.setPresentationAvailable(false);
    c.setContainerVisible(true);
    report("logical reopen while unavailable remains suppressed", win.isHidden());
    c.setPresentationAvailable(true);
    report("availability then restores the new logical reopen", win.isVisible());
    visibility.clear();
    c.setPresentationAvailable(false);
    win.releaseContainer();
    generic->hide();
    c.setPresentationAvailable(true);
    report("released container cannot toggle its former window", win.isHidden());
    c.setPresentationAvailable(false);
    QWidget panel;
    auto* layout = new QVBoxLayout(&panel);
    layout->addWidget(&c);
    c.show();
    panel.show();
    report("docking a suppressed float reapplies the container gate",
           c.isPanelDocked() && c.isHidden() && c.isContainerVisible());
    c.setPresentationAvailable(true);
    report("docked content recovers without losing its body", c.content()->isVisible());
    report("floating availability never emits a persisted visibility change",
           visibility.isEmpty());
    c.setParent(nullptr); // stack-owned; panel must not destroy it first

    ContainerWidget replacement("replacement", "Replacement");
    replacement.setContent(new QLabel("new payload"));
    win.takeContainer(&c);
    generic->show();
    win.takeContainer(&replacement);
    c.setPresentationAvailable(false);
    report("replaced container cannot suppress the new window owner", win.isVisible());
    replacement.setPresentationAvailable(false);
    report("only the current hosted container gates its window", win.isHidden());
    win.releaseContainer();
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

    win.show();
    c.setContainerVisible(false);
    report("unmanaged float retains its existing outer-window behavior",
           win.isVisible() && !c.isPresentationManaged());
    c.setContainerVisible(true);

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

// #3451: a width-capped applet (e.g. Antenna Genius, max 260) should have its
// cap lifted while floating so it fills the window instead of hugging the left
// edge, and have the cap restored when it docks back into the panel strip.
void testFloatingWidthPolicy()
{
    ContainerWidget c("id", "T");
    auto* body = new QLabel("payload");
    body->setMaximumWidth(260);   // mimic a width-capped applet
    c.setContent(body);

    report("docked content keeps its width cap",
           body->maximumWidth() == 260);

    FloatingContainerWindow win;
    win.takeContainer(&c);
    report("floating content cap is lifted to fill the window",
           body->maximumWidth() == QWIDGETSIZE_MAX);

    win.releaseContainer();
    report("docked-again content cap is restored",
           body->maximumWidth() == 260);

    // An uncapped applet (e.g. the Amplifier) stays uncapped throughout.
    ContainerWidget c2("id2", "T2");
    auto* body2 = new QLabel("uncapped");
    c2.setContent(body2);
    FloatingContainerWindow win2;
    win2.takeContainer(&c2);
    report("uncapped content stays uncapped while floating",
           body2->maximumWidth() == QWIDGETSIZE_MAX);
    win2.releaseContainer();
    report("uncapped content stays uncapped after docking",
           body2->maximumWidth() == QWIDGETSIZE_MAX);
}

void testDefaultFloatingSize()
{
    ContainerWidget c("sized", "Sized");
    c.setContent(new QLabel("payload"));
    c.setDefaultFloatingSize(QSize(640, 445));

    FloatingContainerWindow win;
    win.takeContainer(&c);
    win.restoreAndEnsureVisible(nullptr);
    report("container-specific first-float size overrides generic fallback",
           win.size() == QSize(640, 445));
    win.releaseContainer();
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
    TestSettingsProfile settingsProfile(QStringLiteral("aether-container-widget-test"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    std::printf("Container system Phase 1 test harness\n\n");

    testContainerBasics();
    testSetContent();
    testVisibilitySignal();
    testTransientPresentation();
    testFloatingPresentation();
    testFloatDockCycle();
    testFloatingWidthPolicy();
    testDefaultFloatingSize();
    testCloseSignal();
    testTitlebarCloseButtonToggle();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
