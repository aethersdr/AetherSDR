// Construction smoke test for the System Info dialog (#2554).
//
// Written because the dialog shipped with a null dereference that a full build
// and 326 passing tests did not catch: rebuildCategoryFilters() reached through
// m_logViewer for a parent widget before m_logViewer existed, so the dialog
// crashed the instant it was constructed. Nothing in the suite constructed it.
//
// This is deliberately shallow — construct, show, hide, destroy — because that
// is the path that was broken. Depth can come later; coverage of the path an
// operator takes by clicking the menu item cannot.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/SystemInfoDialog.h"

#include <QApplication>
#include <QTabWidget>
#include <QTableWidget>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void report(const char* what, bool ok)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    TestSettingsProfile profile(QStringLiteral("aether-system-info-dialog-test"));

    // Construction alone is the regression this guards.
    SystemInfoDialog dialog;
    report("the dialog constructs without crashing", true);

    auto* tabs = dialog.findChild<QTabWidget*>();
    report("it has a tab widget", tabs != nullptr);
    if (tabs != nullptr) {
        report("it has exactly two tabs", tabs->count() == 2);
        report("first tab is Threads", tabs->tabText(0) == QLatin1String("Threads"));
        report("second tab is Logs", tabs->tabText(1) == QLatin1String("Logs"));
    }

    auto* table = dialog.findChild<QTableWidget*>();
    report("the thread table exists", table != nullptr);
    if (table != nullptr) {
        // Four data columns plus a trailing spacer that absorbs slack on a wide
        // window — assert the shape, not a magic number, so a future column is a
        // deliberate edit here rather than a silent count change.
        report("the table has four data columns plus a spacer", table->columnCount() == 5);
        report("column 0 is Thread",
               table->horizontalHeaderItem(0)->text() == QLatin1String("Thread"));
        report("column 1 is TID",
               table->horizontalHeaderItem(1)->text() == QLatin1String("TID"));
        report("column 2 is CPU %",
               table->horizontalHeaderItem(2)->text() == QLatin1String("CPU %"));
        report("column 3 is Total CPU (s)",
               table->horizontalHeaderItem(3)->text() == QLatin1String("Total CPU (s)"));
        report("the last column is an unlabelled spacer",
               table->horizontalHeaderItem(4)->text().isEmpty());
        report("the table is sortable", table->isSortingEnabled());
    }

    // show() starts the collector thread and opens the log tail; hide() must
    // stop and close them. Both run in showEvent/hideEvent, which is where a
    // teardown mistake would surface as a hang or a crash on close.
    dialog.show();
    QCoreApplication::processEvents();
    report("show() does not crash", true);

    dialog.hide();
    QCoreApplication::processEvents();
    report("hide() stops sampling cleanly", true);

    std::printf("%s\n", g_failures == 0 ? "system_info_dialog_test: all passed"
                                        : "system_info_dialog_test: FAILURES");
    return g_failures == 0 ? 0 : 1;
}
