// Reset Settings' prompts (RFC #6226): the confirmation, a CanonWindow dialog
// with the files in a nested list, and the failure notice, a CanonMessageBox.
// Socket-free and file-free: the prompts only ask and report; SupportDialog
// does the removal and is not linked here.
//
// The confirmation guards a destructive action, so every way of dismissing it
// without clicking Yes (Return, Escape, Cancel, the corner button, Return on a
// focused Yes) must leave the settings alone.

#include "TestSettingsProfile.h"
#include "gui/CanonMessageBox.h"
#include "gui/CanonWindow.h"
#include "gui/ResetSettingsPrompts.h"

#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTest>
#include <QTimer>
#include <QToolButton>
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

CanonWindow* openBox(const char* objectName)
{
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (w->objectName() == QLatin1String(objectName) && w->isVisible()) {
            return qobject_cast<CanonWindow*>(w);
        }
    }
    return nullptr;
}

// Runs `act` against the box once the blocking call below has shown it. If
// the box never appears, the act is skipped and a later check fails; the
// fallback reject keeps a broken box from hanging the test.
void whenShown(const char* objectName, std::function<void(CanonWindow*)> act)
{
    QTimer::singleShot(0, [objectName, act] {
        CanonWindow* box = openBox(objectName);
        check(box != nullptr, "the prompt is a visible canon window");
        if (!box) return;
        QTest::qWaitForWindowExposed(box);
        act(box);
        if (box->isVisible()) box->reject();
    });
}

QPushButton* button(const CanonWindow* box, const char* objectName)
{
    return box->findChild<QPushButton*>(QLatin1String(objectName));
}

QLabel* infoLabel(const CanonWindow* box)
{
    return box->findChild<QLabel*>(QStringLiteral("canonMessageInfo"));
}

QListWidget* fileList(const CanonWindow* box)
{
    return box->findChild<QListWidget*>(QStringLiteral("resetSettingsFiles"));
}

QStringList listItems(const QListWidget* list)
{
    QStringList items;
    for (int i = 0; list && i < list->count(); ++i) items << list->item(i)->text();
    return items;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("reset-settings-prompts");
    QApplication app(argc, argv);

    const QString backups = QStringLiteral("/tmp/backups");
    const QStringList paths = {QStringLiteral("/tmp/a/AetherSDR.db"),
                               QStringLiteral("/tmp/a/AetherSDR.db-wal")};
    constexpr const char* kConfirm = "resetSettingsConfirmation";
    constexpr const char* kFailures = "resetSettingsFailures";

    QStringList shown;
    bool selectable = false;
    whenShown(kConfirm, [&](CanonWindow* box) {
        QPushButton* cancel = button(box, "resetSettingsCancel");
        check(cancel && cancel->isDefault(), "Cancel is the default button");
        check(cancel && box->focusWidget() == cancel, "Cancel takes focus");
        QListWidget* list = fileList(box);
        shown = listItems(list);
        selectable = list && list->selectionMode() != QAbstractItemView::NoSelection;
        QTest::keyClick(box->focusWidget() ? box->focusWidget() : box, Qt::Key_Return);
    });
    check(!ResetSettingsPrompts::confirm(nullptr, backups, paths), "Return cancels the reset");
    check(shown == paths, "the files to remove are listed as given, in a nested list");
    check(selectable, "the listed paths can be selected and copied");

    whenShown(kConfirm, [](CanonWindow* box) { QTest::keyClick(box, Qt::Key_Escape); });
    check(!ResetSettingsPrompts::confirm(nullptr, backups, paths), "Escape cancels the reset");

    whenShown(kConfirm, [](CanonWindow* box) {
        if (auto* b = button(box, "resetSettingsCancel")) b->click();
    });
    check(!ResetSettingsPrompts::confirm(nullptr, backups, paths), "Cancel cancels the reset");

    whenShown(kConfirm, [](CanonWindow* box) {
        // The corner button is the window's only QToolButton.
        if (auto* close = box->findChild<QToolButton*>()) close->click();
    });
    check(!ResetSettingsPrompts::confirm(nullptr, backups, paths),
          "the corner button cancels the reset");

    whenShown(kConfirm, [](CanonWindow* box) {
        if (auto* yes = button(box, "resetSettingsConfirm")) {
            yes->setFocus();
            QTest::keyClick(yes, Qt::Key_Return);
        }
    });
    check(!ResetSettingsPrompts::confirm(nullptr, backups, paths),
          "Return on a focused Yes does not confirm");

    whenShown(kConfirm, [](CanonWindow* box) {
        if (auto* b = button(box, "resetSettingsConfirm")) b->click();
    });
    check(ResetSettingsPrompts::confirm(nullptr, backups, paths), "only Yes confirms the reset");

    shown.clear();
    whenShown(kConfirm, [&](CanonWindow* box) {
        shown = listItems(fileList(box));
        QTest::keyClick(box, Qt::Key_Escape);
    });
    ResetSettingsPrompts::confirm(nullptr, backups, {});
    check(shown == QStringList{QStringLiteral("None")}, "an empty file list says None");

    QString info;
    bool isMessageBox = false;
    bool okClosed = false;
    whenShown(kFailures, [&](CanonWindow* box) {
        isMessageBox = qobject_cast<CanonMessageBox*>(box) != nullptr;
        if (QLabel* label = infoLabel(box)) info = label->text();
        if (auto* b = button(box, "resetSettingsOk")) {
            b->click();
            okClosed = !box->isVisible();
        }
    });
    ResetSettingsPrompts::showFailures(nullptr, {paths.at(1)}, {paths.at(0)});
    check(info.contains(QStringLiteral("Failed:\n- /tmp/a/AetherSDR.db\n")),
          "the failure notice lists the files that failed");
    check(info.endsWith(QStringLiteral("Removed:\n- /tmp/a/AetherSDR.db-wal")),
          "the failure notice lists the files that were removed");
    check(isMessageBox, "the failure notice is a CanonMessageBox");
    check(okClosed, "OK closes the failure notice");

    return g_failures ? 1 : 0;
}
