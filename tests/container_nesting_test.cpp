// Phase 3 tests — the 8 nested-float/dock/hide edge cases spelled
// out in the container-system plan.  Each test builds a small tree,
// drives a specific sequence of manager calls, and asserts the
// resulting state.

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
    std::printf("%s %-72s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name, detail.c_str());
    if (!ok) ++g_failed;
}

// Scaffold: a root QWidget with a vertical layout hosting the
// sidebar container.  Returns the sidebar for convenience.
struct Scaffold {
    QWidget root;
    QVBoxLayout* rootLayout;
    ContainerManager mgr;
    ContainerWidget* sidebar;
    ContainerWidget* txDsp;
    ContainerWidget* chain;
    ContainerWidget* ceq;
    ContainerWidget* cmp;

    Scaffold()
        : rootLayout(new QVBoxLayout(&root))
    {
        rootLayout->setContentsMargins(0, 0, 0, 0);
        sidebar = mgr.createContainer("sidebar", "Sidebar");
        rootLayout->addWidget(sidebar);
        txDsp = mgr.createContainer("tx_dsp", "TX DSP", "", "sidebar");
        chain = mgr.createContainer("chain", "CHAIN", "", "tx_dsp");
        ceq   = mgr.createContainer("ceq",   "CEQ",   "", "tx_dsp");
        cmp   = mgr.createContainer("cmp",   "CMP",   "", "tx_dsp");
    }
};

// ── Edge case 1: float parent while child is floating ──────────
void test_1_floatParentWhileChildFloating()
{
    Scaffold s;
    s.mgr.floatContainer("cmp");
    report("1a: CMP is floating", s.cmp->isFloating());
    report("1a: TX DSP still has 2 docked children",
           s.txDsp->childWidgetCount() == 2);

    s.mgr.floatContainer("tx_dsp");
    report("1b: TX DSP is floating", s.txDsp->isFloating());
    report("1b: CMP still floating (its window unaffected)",
           s.cmp->isFloating());
    report("1b: TX DSP body still has 2 children (CHAIN + CEQ)",
           s.txDsp->childWidgetCount() == 2);
}

// ── Edge case 2: float child while parent is already floating ──
void test_2_floatChildWhileParentFloating()
{
    Scaffold s;
    s.mgr.floatContainer("tx_dsp");
    report("2a: TX DSP floating with 3 docked children",
           s.txDsp->isFloating() && s.txDsp->childWidgetCount() == 3);

    s.mgr.floatContainer("chain");
    report("2b: CHAIN is floating",
           s.chain->isFloating());
    report("2b: TX DSP's body now has 2 children",
           s.txDsp->childWidgetCount() == 2);
    report("2b: TX DSP still floating (unchanged)",
           s.txDsp->isFloating());
}

// ── Edge case 3: hide parent while child is floating ───────────
void test_3_hideParentWhileChildFloating()
{
    Scaffold s;
    s.mgr.floatContainer("chain");
    report("3a: CHAIN is floating", s.chain->isFloating());

    s.txDsp->setContainerVisible(false);
    report("3b: TX DSP invisible",
           !s.txDsp->isContainerVisible());
    report("3b: CHAIN floating window's container still visible",
           s.chain->isContainerVisible());
}

// ── Edge case 4: re-dock child while parent is floating ────────
void test_4_redockChildWhileParentFloating()
{
    Scaffold s;
    s.mgr.floatContainer("tx_dsp");
    s.mgr.floatContainer("chain");
    report("4a: CHAIN floating, TX DSP has 2 docked children",
           s.chain->isFloating() && s.txDsp->childWidgetCount() == 2);

    s.mgr.dockContainer("chain");
    report("4b: CHAIN docked back",
           !s.chain->isFloating());
    report("4b: TX DSP body now contains 3 children again",
           s.txDsp->childWidgetCount() == 3);
    report("4b: TX DSP still floating",
           s.txDsp->isFloating());
    // CHAIN was index 0 originally; ensure it's at slot 0 still.
    report("4b: CHAIN re-inserted at original index 0",
           s.txDsp->childWidgetAt(0) == s.chain,
           QString::asprintf("got=%p want=%p",
               s.txDsp->childWidgetAt(0), s.chain).toStdString());
}

// ── Edge case 5: re-dock parent while child is still floating ──
void test_5_redockParentWhileChildFloating()
{
    Scaffold s;
    s.mgr.floatContainer("cmp");
    s.mgr.floatContainer("tx_dsp");

    // Re-dock parent.  CMP window stays open.
    s.mgr.dockContainer("tx_dsp");
    report("5a: TX DSP docked back into sidebar",
           !s.txDsp->isFloating());
    report("5a: CMP still floating",
           s.cmp->isFloating());
    // TX DSP should hold CHAIN + CEQ (CMP stays out).
    report("5a: TX DSP body has 2 children (CMP still out)",
           s.txDsp->childWidgetCount() == 2);

    // Now re-dock CMP.  It should go back into TX DSP's body at its
    // original slot (index 2).
    s.mgr.dockContainer("cmp");
    report("5b: CMP docked back, TX DSP has 3 children",
           !s.cmp->isFloating() && s.txDsp->childWidgetCount() == 3);
    report("5b: CMP lands at remembered slot index 2",
           s.txDsp->childWidgetAt(2) == s.cmp);
}

// ── Edge case 6: drag onto hidden container ────────────────────
//
// Phase 3 doesn't implement drag-drop yet (Phase 4 job).  Skipped.

// ── Edge case 7: app close with mixed state ────────────────────
void test_7_closeRestoreMixedState()
{
    AppSettings::instance().setValue("ContainerTree", "");

    // Build, create a messy state, save.
    {
        Scaffold s;
        s.mgr.floatContainer("cmp");        // CMP floating
        s.mgr.floatContainer("tx_dsp");      // TX DSP floating (CMP still floating)
        s.ceq->setContainerVisible(false);  // CEQ hidden inside TX DSP
        s.mgr.saveState();
    }

    // Fresh manager, restore.
    QWidget root;
    auto* rootLayout = new QVBoxLayout(&root);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    ContainerManager mgr;
    // Pre-create the sidebar — it's the top-level container that the
    // app's MainWindow would have inserted into the splitter.  Restore
    // then populates everything nested inside.
    auto* sidebar = mgr.createContainer("sidebar", "Sidebar");
    rootLayout->addWidget(sidebar);
    mgr.restoreState();

    auto* txDsp = mgr.container("tx_dsp");
    auto* cmp   = mgr.container("cmp");
    auto* ceq   = mgr.container("ceq");
    auto* chain = mgr.container("chain");
    report("7: restore rebuilt all four containers",
           txDsp && cmp && ceq && chain);
    report("7: TX DSP floating state restored",
           txDsp && txDsp->isFloating());
    report("7: CMP floating state restored",
           cmp && cmp->isFloating());
    report("7: CEQ hidden state restored",
           ceq && !ceq->isContainerVisible());
    report("7: CHAIN visible + docked",
           chain && chain->isContainerVisible() && chain->isPanelDocked());
}

// ── Edge case 8: destroying a container with floated children ──
void test_8_destroyWithFloatedChildren()
{
    Scaffold s;
    s.mgr.floatContainer("cmp");
    s.mgr.floatContainer("chain");
    report("8a: pre: CMP + CHAIN floating, TX DSP has 1 docked child",
           s.cmp->isFloating() && s.chain->isFloating()
               && s.txDsp->childWidgetCount() == 1);

    s.mgr.destroyContainer("tx_dsp");

    // Everything under tx_dsp should be gone.
    report("8b: tx_dsp destroyed",  s.mgr.container("tx_dsp") == nullptr);
    report("8b: chain destroyed",   s.mgr.container("chain") == nullptr);
    report("8b: ceq destroyed",     s.mgr.container("ceq")   == nullptr);
    report("8b: cmp destroyed",     s.mgr.container("cmp")   == nullptr);
    // Sidebar still alive.
    report("8b: sidebar survives",  s.mgr.container("sidebar") != nullptr);
}

// ── Bonus: reparent a container between other containers ──────
void test_reparent()
{
    Scaffold s;
    // Create a second sibling of tx_dsp so we can move a child there.
    auto* other = s.mgr.createContainer("other", "Other", "", "sidebar");
    report("pre: other has 0 children, tx_dsp has 3",
           other->childWidgetCount() == 0
               && s.txDsp->childWidgetCount() == 3);

    // Move CEQ from tx_dsp → other.
    s.mgr.reparentContainer("ceq", "other", -1);
    report("reparent: CEQ is child of other",
           s.mgr.parentOf("ceq") == "other");
    report("reparent: tx_dsp down to 2 children",
           s.txDsp->childWidgetCount() == 2);
    report("reparent: other has 1 child",
           other->childWidgetCount() == 1);
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    std::printf("Container system Phase 3 nesting tests\n\n");

    test_1_floatParentWhileChildFloating();
    test_2_floatChildWhileParentFloating();
    test_3_hideParentWhileChildFloating();
    test_4_redockChildWhileParentFloating();
    test_5_redockParentWhileChildFloating();
    // Case 6 (drag onto hidden) = Phase 4.
    test_7_closeRestoreMixedState();
    test_8_destroyWithFloatedChildren();
    test_reparent();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
