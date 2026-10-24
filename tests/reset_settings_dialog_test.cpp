// Reset Settings' CanonWindow dialogs (RFC #6226). Socket-free and file-free:
// the dialogs only ask and report; SupportDialog does the removal and is not
// linked here.
//
// The confirmation guards a destructive action, so the keys that dismiss a
// dialog without reading it (Return, Escape) must leave the settings alone,
// and only the Yes button may confirm.

#include "TestSettingsProfile.h"
#include "gui/CanonWindow.h"
#include "gui/ResetSettingsDialog.h"

#include <QApplication>
#include <QListWidget>
#include <QPushButton>
#include <QTest>
#include <QTimer>
#include <cstdio>
#include <functional>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* name)
{
    std::printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
    g_failures += !ok;
}

CanonWindow* openDialog()
{
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (w->objectName() == QLatin1String("resetSettingsDialog") && w->isVisible()) {
            return qobject_cast<CanonWindow*>(w);
        }
    }
    return nullptr;
}

// Runs `act` against the dialog once the blocking call below has shown it.
// If the dialog never appears, the act is skipped and a later check fails;
// the fallback reject keeps a broken dialog from hanging the test.
void whenShown(std::function<void(CanonWindow*)> act)
{
    QTimer::singleShot(0, [act] {
        CanonWindow* dlg = openDialog();
        check(dlg != nullptr, "the dialog is a visible CanonWindow named resetSettingsDialog");
        if (!dlg) return;
        act(dlg);
        if (dlg->isVisible()) dlg->reject();
    });
}

QStringList listItems(const CanonWindow* dlg, const char* objectName)
{
    QStringList items;
    if (auto* list = dlg->findChild<QListWidget*>(QLatin1String(objectName))) {
        for (int i = 0; i < list->count(); ++i) items << list->item(i)->text();
    }
    return items;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("reset-settings-dialog");
    QApplication app(argc, argv);

    const QStringList paths = {QStringLiteral("/tmp/a/AetherSDR.db"),
                               QStringLiteral("/tmp/a/AetherSDR.db-wal")};

    QStringList shown;
    whenShown([&](CanonWindow* dlg) {
        shown = listItems(dlg, "resetSettingsFiles");
        auto* cancel = dlg->findChild<QPushButton*>(QStringLiteral("resetSettingsCancel"));
        check(cancel && cancel->isDefault(), "Cancel is the default button");
        QTest::keyClick(dlg, Qt::Key_Return);
    });
    check(!ResetSettingsDialog::confirm(nullptr, QStringLiteral("/tmp/backups"), paths),
          "Return cancels the reset");
    check(shown == paths, "the files to remove are listed as given");

    whenShown([](CanonWindow* dlg) { QTest::keyClick(dlg, Qt::Key_Escape); });
    check(!ResetSettingsDialog::confirm(nullptr, QStringLiteral("/tmp/backups"), paths),
          "Escape cancels the reset");

    whenShown([](CanonWindow* dlg) {
        if (auto* b = dlg->findChild<QPushButton*>(QStringLiteral("resetSettingsCancel"))) b->click();
    });
    check(!ResetSettingsDialog::confirm(nullptr, QStringLiteral("/tmp/backups"), paths),
          "Cancel cancels the reset");

    whenShown([](CanonWindow* dlg) {
        if (auto* b = dlg->findChild<QPushButton*>(QStringLiteral("resetSettingsConfirm"))) b->click();
    });
    check(ResetSettingsDialog::confirm(nullptr, QStringLiteral("/tmp/backups"), paths),
          "only Yes confirms the reset");

    shown.clear();
    whenShown([&](CanonWindow* dlg) {
        shown = listItems(dlg, "resetSettingsFiles");
        QTest::keyClick(dlg, Qt::Key_Escape);
    });
    ResetSettingsDialog::confirm(nullptr, QStringLiteral("/tmp/backups"), {});
    check(shown == QStringList{QStringLiteral("None")}, "an empty file list says None");

    QStringList failed;
    QStringList removed;
    bool okClosed = false;
    whenShown([&](CanonWindow* dlg) {
        failed = listItems(dlg, "resetSettingsFailed");
        removed = listItems(dlg, "resetSettingsRemoved");
        if (auto* b = dlg->findChild<QPushButton*>(QStringLiteral("resetSettingsOk"))) {
            b->click();
            okClosed = !dlg->isVisible();
        }
    });
    ResetSettingsDialog::showFailures(nullptr, {paths.at(1)}, {paths.at(0)});
    check(failed == QStringList{paths.at(0)}, "the failure notice lists the files that failed");
    check(removed == QStringList{paths.at(1)}, "the failure notice lists the files that were removed");
    check(okClosed, "OK closes the failure notice");

    return g_failures ? 1 : 0;
}
