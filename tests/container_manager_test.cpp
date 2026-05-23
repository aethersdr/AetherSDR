// Phase 2 tests — ContainerManager lifecycle, factory, persistence.
// Headless via QApplication with offscreen platform.

#include "gui/containers/ContainerManager.h"
#include "gui/containers/ContainerWidget.h"
#include "core/AppSettings.h"

#include <QApplication>
#include <QLabel>
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

void testCreateAndLookup()
{
    ContainerManager m;
    auto* c = m.createContainer("one", "One");
    report("create returns non-null",     c != nullptr);
    report("registered under id",          m.container("one") == c);
    report("count is 1",                   m.containerCount() == 1);
    report("unknown id returns null",      m.container("bogus") == nullptr);
    m.destroyContainer("one");
    report("destroy removes from registry", m.container("one") == nullptr);
    report("count drops to 0",             m.containerCount() == 0);
}

void testDoubleCreateIdempotent()
{
    ContainerManager m;
    auto* a = m.createContainer("x", "X");
    auto* b = m.createContainer("x", "X");
    report("double-create returns same instance", a == b);
    report("count stays at 1",                    m.containerCount() == 1);
}

void testContentFactory()
{
    ContainerManager m;
    int calls = 0;
    m.registerContent("TestContent", [&](const QString& id) {
        ++calls;
        auto* w = new QLabel("hello");
        w->setObjectName(id);
        return w;
    });

    auto* c = m.createContainer("t1", "T1", "TestContent");
    report("factory invoked once",      calls == 1);
    report("content installed",         c->content() != nullptr);
    report("content has correct parent",
           c->content()->parentWidget() != nullptr);

    // No factory registered for this type → content stays null.
    auto* c2 = m.createContainer("t2", "T2", "MissingType");
    report("missing factory leaves content null",
           c2->content() == nullptr);
}

void testFloatDockViaManager()
{
    ContainerManager m;

    // Simulate a panel parent with a vertical layout — this is what
    // AppletPanel will hand us in Phase 4.
    QWidget panel;
    auto* layout = new QVBoxLayout(&panel);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* c = m.createContainer("c1", "C1");
    layout->addWidget(c);

    // Float.
    m.floatContainer("c1");
    report("float transitions dockMode",    c->isFloating());
    report("container removed from panel layout",
           layout->indexOf(c) == -1);

    // Dock.
    m.dockContainer("c1");
    report("dock transitions back to panel", c->isPanelDocked());
    report("container reinserted into panel layout",
           layout->indexOf(c) >= 0);
}

void testSaveRestoreRoundTrip()
{
    // Use a distinct settings state — the global singleton persists
    // across tests but we only inspect our own key.
    AppSettings::instance().setValue("ContainerTree", "");

    {
        ContainerManager m;
        m.registerContent("Leaf", [](const QString& id) {
            auto* w = new QLabel(id);
            return w;
        });
        m.createContainer("alpha", "Alpha", "Leaf");
        m.createContainer("beta",  "Beta",  "Leaf");
        m.container("beta")->setContainerVisible(false);
        m.saveState();
    }

    // Fresh manager; restore should recreate containers + state.
    ContainerManager m2;
    int factoryCalls = 0;
    m2.registerContent("Leaf", [&](const QString& id) {
        ++factoryCalls;
        return new QLabel(id);
    });
    m2.restoreState();

    auto* alpha = m2.container("alpha");
    auto* beta  = m2.container("beta");
    report("restore recreated alpha",         alpha != nullptr);
    report("restore recreated beta",          beta  != nullptr);
    report("factory invoked during restore",  factoryCalls == 2,
           std::to_string(factoryCalls));
    report("alpha visible state restored",    alpha && alpha->isContainerVisible());
    report("beta hidden state restored",      beta  && !beta->isContainerVisible());
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    std::printf("Container system Phase 2 manager tests\n\n");

    testCreateAndLookup();
    testDoubleCreateIdempotent();
    testContentFactory();
    testFloatDockViaManager();
    testSaveRestoreRoundTrip();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
