// Standalone test harness for HelpDialog guide search.
// Build: CMake target `help_dialog_test`. Exit 0 = pass.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/HelpDialog.h"

#include <QApplication>
#include <QDesktopServices>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QTemporaryFile>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTest>
#include <QUrl>
#include <cstdio>
#include <string>

using namespace AetherSDR;

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

// Stands in for the system browser: QDesktopServices hands it every https URL.
class UrlCatcher : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void handle(const QUrl& url) { urls.append(url); }
};

// Click the anchor whose visible text contains `linkText` in the browser, the
// way an operator would, and return what the browser asked the OS to open.
QList<QUrl> clickLink(QTextBrowser* browser, UrlCatcher& catcher, const QString& linkText)
{
    catcher.urls.clear();
    QTextCursor cursor = browser->document()->find(linkText);
    if (cursor.isNull())
        return {};
    const int mid = (cursor.selectionStart() + cursor.selectionEnd()) / 2;
    cursor.setPosition(mid);
    browser->ensureCursorVisible();
    browser->setTextCursor(cursor);
    browser->ensureCursorVisible();
    QCoreApplication::processEvents();
    const QPoint pos = browser->cursorRect(cursor).center();
    QTest::mouseClick(browser->viewport(), Qt::LeftButton, Qt::NoModifier, pos);
    QCoreApplication::processEvents();
    return catcher.urls;
}

// HelpDialog's QTextBrowser has openExternalLinks on: an http(s) link opens in
// the system browser and the dialog stays on its guide. Uses the real bundled
// page, which carries both a docs.aethersdr.com and a GitHub link.
void testExternalLinksOpenInSystemBrowser()
{
    const QString page = QStringLiteral(AETHER_SOURCE_DIR)
                         + QStringLiteral("/resources/help/contributing-to-aethersdr.md");
    report("bundled contributing page exists", QFile::exists(page));

    UrlCatcher catcher;
    QDesktopServices::setUrlHandler(QStringLiteral("https"), &catcher, "handle");

    HelpDialog dialog("Contributing to AetherSDR", page);
    dialog.show();
    QCoreApplication::processEvents();
    HelpWidgets widgets = findWidgets(dialog);
    if (!requireWidgets(widgets)) {
        QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
        return;
    }
    const QString before = widgets.browser->toPlainText();

    const QList<QUrl> docs = clickLink(widgets.browser, catcher,
                                       QStringLiteral("docs.aethersdr.com/contributing-guide"));
    report("docs link opens in the system browser",
           docs == QList<QUrl>{QUrl(QStringLiteral("https://docs.aethersdr.com/contributing-guide"))},
           docs.isEmpty() ? "(nothing opened)" : docs.first().toString().toStdString());

    const QList<QUrl> github = clickLink(widgets.browser, catcher,
                                         QStringLiteral("github.com/aethersdr/AetherSDR"));
    report("GitHub link opens in the system browser",
           github == QList<QUrl>{QUrl(QStringLiteral("https://github.com/aethersdr/AetherSDR"))},
           github.isEmpty() ? "(nothing opened)" : github.first().toString().toStdString());

    report("dialog did not navigate away from the guide",
           widgets.browser->source().isEmpty() && widgets.browser->toPlainText() == before,
           widgets.browser->source().toString().toStdString());

    QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-help-dialog-test"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    std::printf("HelpDialog find test harness\n\n");

    testFindNextAndWrap();
    testNoMatchClearsSelectionAndRecovers();
    testReturnPressedFindsNext();
    testEmptyQueryDisablesFind();
    testExternalLinksOpenInSystemBrowser();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}

#include "help_dialog_test.moc"
