// Standalone test harness for HelpDialog guide search.
// Build: CMake target `help_dialog_test`. Exit 0 = pass.

#include "gui/HelpDialog.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QTemporaryFile>
#include <QTextBrowser>
#include <QTextCursor>
#include <cstdio>
#include <string>

using namespace MasterSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-56s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

bool writeTempHelp(QTemporaryFile& file)
{
    if (!file.open())
        return false;

    const QByteArray body =
        "target first\n\n"
        "middle line\n\n"
        "target second\n\n"
        "end marker\n";
    const bool ok = file.write(body) == body.size() && file.flush();
    file.close();
    return ok;
}

struct HelpWidgets {
    QLineEdit* edit{nullptr};
    QPushButton* button{nullptr};
    QLabel* status{nullptr};
    QTextBrowser* browser{nullptr};
};

HelpWidgets findWidgets(HelpDialog& dialog)
{
    return {
        dialog.findChild<QLineEdit*>("helpFindEdit"),
        dialog.findChild<QPushButton*>("helpFindButton"),
        dialog.findChild<QLabel*>("helpFindStatus"),
        dialog.findChild<QTextBrowser*>("helpBrowser")
    };
}

bool requireWidgets(const HelpWidgets& widgets)
{
    const bool ok = widgets.edit && widgets.button && widgets.status && widgets.browser;
    report("find widgets are present", ok);
    return ok;
}

bool selectedTextEquals(QTextBrowser* browser, const QString& expected)
{
    return browser->textCursor().selectedText().compare(expected, Qt::CaseInsensitive) == 0;
}

void testFindNextAndWrap()
{
    QTemporaryFile file;
    const bool wrote = writeTempHelp(file);
    report("temp help document created", wrote);
    if (!wrote)
        return;

    HelpDialog dialog("Search Test", file.fileName());
    HelpWidgets widgets = findWidgets(dialog);
    if (!requireWidgets(widgets))
        return;

    widgets.edit->setText("target");
    report("find button enables with query", widgets.button->isEnabled());

    widgets.button->click();
    const QTextCursor firstCursor = widgets.browser->textCursor();
    const int firstStart = firstCursor.selectionStart();
    report("first find selects match", selectedTextEquals(widgets.browser, "target"));

    widgets.button->click();
    const QTextCursor secondCursor = widgets.browser->textCursor();
    const int secondStart = secondCursor.selectionStart();
    report("find next advances", selectedTextEquals(widgets.browser, "target") && secondStart > firstStart,
           "first=" + std::to_string(firstStart) + " second=" + std::to_string(secondStart));

    widgets.button->click();
    const QTextCursor wrapCursor = widgets.browser->textCursor();
    report("find next wraps to first match",
           selectedTextEquals(widgets.browser, "target") && wrapCursor.selectionStart() == firstStart);
    report("wrap status is shown", widgets.status->text() == "Wrapped to top",
           widgets.status->text().toStdString());
}

void testNoMatchClearsSelectionAndRecovers()
{
    QTemporaryFile file;
    const bool wrote = writeTempHelp(file);
    report("temp help document created", wrote);
    if (!wrote)
        return;

    HelpDialog dialog("Search Test", file.fileName());
    HelpWidgets widgets = findWidgets(dialog);
    if (!requireWidgets(widgets))
        return;

    widgets.edit->setText("absent");
    widgets.button->click();
    report("no match status is shown", widgets.status->text() == "No matches",
           widgets.status->text().toStdString());
    report("no match leaves no selection", !widgets.browser->textCursor().hasSelection());

    widgets.edit->setText("middle");
    report("query change clears status", widgets.status->text().isEmpty(),
           widgets.status->text().toStdString());
    report("query change clears selection", !widgets.browser->textCursor().hasSelection());

    widgets.button->click();
    report("search recovers after no match", selectedTextEquals(widgets.browser, "middle"));
}

void testReturnPressedFindsNext()
{
    QTemporaryFile file;
    const bool wrote = writeTempHelp(file);
    report("temp help document created", wrote);
    if (!wrote)
        return;

    HelpDialog dialog("Search Test", file.fileName());
    HelpWidgets widgets = findWidgets(dialog);
    if (!requireWidgets(widgets))
        return;

    widgets.edit->setText("target");
    const bool invoked = QMetaObject::invokeMethod(widgets.edit, "returnPressed", Qt::DirectConnection);
    report("returnPressed signal invoked", invoked);
    report("Return finds first match", selectedTextEquals(widgets.browser, "target"));
}

void testEmptyQueryDisablesFind()
{
    QTemporaryFile file;
    const bool wrote = writeTempHelp(file);
    report("temp help document created", wrote);
    if (!wrote)
        return;

    HelpDialog dialog("Search Test", file.fileName());
    HelpWidgets widgets = findWidgets(dialog);
    if (!requireWidgets(widgets))
        return;

    report("find button starts disabled", !widgets.button->isEnabled());
    widgets.edit->setText("target");
    report("find button enables", widgets.button->isEnabled());
    widgets.edit->clear();
    report("find button disables when query clears", !widgets.button->isEnabled());
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    std::printf("HelpDialog find test harness\n\n");

    testFindNextAndWrap();
    testNoMatchClearsSelectionAndRecovers();
    testReturnPressedFindsNext();
    testEmptyQueryDisablesFind();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
