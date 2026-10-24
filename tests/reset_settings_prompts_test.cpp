// Reset Settings' CanonMessageBox prompts (RFC #6226). Socket-free and
// file-free: the prompts only ask and report; SupportDialog does the removal
// and is not linked here.
//
// The confirmation guards a destructive action, so every way of dismissing it
// without clicking Yes (Return, Escape, Cancel, the corner button, Return on a
// focused Yes) must leave the settings alone.

#include "TestSettingsProfile.h"
#include "gui/CanonMessageBox.h"
#include "gui/ResetSettingsPrompts.h"

#include <QApplication>
#include <QLabel>
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

CanonMessageBox* openBox(const char* objectName)
{
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (w->objectName() == QLatin1String(objectName) && w->isVisible()) {
            return qobject_cast<CanonMessageBox*>(w);
        }
    }
    return nullptr;
}

// Runs `act` against the box once the blocking call below has shown it. If
// the box never appears, the act is skipped and a later check fails; the
// fallback reject keeps a broken box from hanging the test.
void whenShown(const char* objectName, std::function<void(CanonMessageBox*)> act)
{
    QTimer::singleShot(0, [objectName, act] {
        CanonMessageBox* box = openBox(objectName);
        check(box != nullptr, "the prompt is a visible CanonMessageBox");
        if (!box) return;
        QTest::qWaitForWindowExposed(box);
        act(box);
        if (box->isVisible()) box->reject();
    });
}

QPushButton* button(const CanonMessageBox* box, const char* objectName)
{
    return box->findChild<QPushButton*>(QLatin1String(objectName));
}

QLabel* infoLabel(const CanonMessageBox* box)
{
    return box->findChild<QLabel*>(QStringLiteral("canonMessageInfo"));
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

    QString info;
    bool selectable = false;
    whenShown(kConfirm, [&](CanonMessageBox* box) {
        QPushButton* cancel = button(box, "resetSettingsCancel");
        check(cancel && cancel->isDefault(), "Cancel is the default button");
        check(cancel && box->focusWidget() == cancel, "Cancel takes focus");
        if (QLabel* label = infoLabel(box)) {
            info = label->text();
            selectable = label->textInteractionFlags() & Qt::TextSelectableByMouse;
        }
        QTest::keyClick(box->focusWidget() ? box->focusWidget() : box, Qt::Key_Return);
    });
    check(!ResetSettingsPrompts::confirm(nullptr, backups, paths), "Return cancels the reset");
    check(info.contains(QStringLiteral("- /tmp/a/AetherSDR.db\n- /tmp/a/AetherSDR.db-wal")),
          "the files to remove are listed as given");
    check(selectable, "the listed paths can be selected and copied");

    whenShown(kConfirm, [](CanonMessageBox* box) { QTest::keyClick(box, Qt::Key_Escape); });
    check(!ResetSettingsPrompts::confirm(nullptr, backups, paths), "Escape cancels the reset");

    whenShown(kConfirm, [](CanonMessageBox* box) {
        if (auto* b = button(box, "resetSettingsCancel")) b->click();
    });
    check(!ResetSettingsPrompts::confirm(nullptr, backups, paths), "Cancel cancels the reset");

    whenShown(kConfirm, [](CanonMessageBox* box) {
        // The corner button is the window's only QToolButton.
        if (auto* close = box->findChild<QToolButton*>()) close->click();
    });
    check(!ResetSettingsPrompts::confirm(nullptr, backups, paths),
          "the corner button cancels the reset");

    whenShown(kConfirm, [](CanonMessageBox* box) {
        if (auto* yes = button(box, "resetSettingsConfirm")) {
            yes->setFocus();
            QTest::keyClick(yes, Qt::Key_Return);
        }
    });
    check(!ResetSettingsPrompts::confirm(nullptr, backups, paths),
          "Return on a focused Yes does not confirm");

    whenShown(kConfirm, [](CanonMessageBox* box) {
        if (auto* b = button(box, "resetSettingsConfirm")) b->click();
    });
    check(ResetSettingsPrompts::confirm(nullptr, backups, paths), "only Yes confirms the reset");

    info.clear();
    whenShown(kConfirm, [&](CanonMessageBox* box) {
        if (QLabel* label = infoLabel(box)) info = label->text();
        QTest::keyClick(box, Qt::Key_Escape);
    });
    ResetSettingsPrompts::confirm(nullptr, backups, {});
    check(info.startsWith(QStringLiteral("Files to remove:\nNone")), "an empty file list says None");

    info.clear();
    bool okClosed = false;
    whenShown(kFailures, [&](CanonMessageBox* box) {
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
    check(okClosed, "OK closes the failure notice");

    return g_failures ? 1 : 0;
}
