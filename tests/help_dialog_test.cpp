// Standalone test harness for HelpDialog guide search.
// Build: CMake target `help_dialog_test`. Exit 0 = pass.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/CanonWindow.h"
#include "gui/HelpDialog.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QKeySequence>
#include <QPalette>
#include <QPointer>
#include <QTextBlock>
#include <QTextFragment>
#include <QToolButton>
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
    // The no-match state is a property the canon sheet matches, drawn in
    // color.accent.danger, not a swapped hex sheet (#7049).
    widgets.status->ensurePolished();
    report("no match marks the status and the field",
           widgets.status->property("noMatch").toBool() && widgets.edit->property("noMatch").toBool());
    report("the no-match status draws in color.accent.danger",
           widgets.status->palette().color(QPalette::WindowText)
               == AetherSDR::ThemeManager::instance().color(widgets.status, QStringLiteral("color.accent.danger")));
    report("no match leaves no selection", !widgets.browser->textCursor().hasSelection());

    widgets.edit->setText("middle");
    report("query change clears status", widgets.status->text().isEmpty(),
           widgets.status->text().toStdString());
    report("query change clears the no-match marks",
           !widgets.status->property("noMatch").toBool() && !widgets.edit->property("noMatch").toBool());
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
// The guide on the style guide's CanonWindow (#7049): a workspace in the
// canon/help scope with the canon header, no footer Close, links in canon cyan
// in each theme, every close path, a legible floor, and the per-guide geometry
// key carried over.
void testCanonWindow()
{
    QTemporaryFile file;
    if (!writeTempHelp(file)) { report("temp help document created", false); return; }
    auto& tm = AetherSDR::ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        report(theme, tm.setActiveTheme(QLatin1String(theme)));
        HelpDialog dialog("Canon Test", file.fileName());
        report("the guide is a CanonWindow", qobject_cast<AetherSDR::CanonWindow*>(&dialog) != nullptr);
        report("it is a frameless workspace window",
               dialog.windowType() == Qt::Window && dialog.windowFlags().testFlag(Qt::FramelessWindowHint));
        report("it uses the canon/help scope", dialog.property("themeContainer").toString() == "canon/help");
        auto* header = dialog.findChild<QLabel*>("canonHeaderTitle");
        report("the canon header carries the guide's title", header && header->text() == "Canon Test");
        bool closeButton = false;
        for (QPushButton* b : dialog.findChildren<QPushButton*>())
            closeButton = closeButton || b->text() == "Close";
        report("the footer Close is gone", !closeButton);
        const QString sheet = dialog.bodyWidget()->styleSheet();
        report("the sheet resolves, with menu and selection rules",
               !sheet.contains(": ;") && sheet.contains("QMenu") && sheet.contains("selection-color"));
        bool literal = false;
        for (QWidget* w : dialog.findChildren<QWidget*>())
            for (const char* hex : {"#111d29", "#c65050", "#203040", "#e07070", "#245a7a"})
                literal = literal || w->styleSheet().contains(QLatin1String(hex), Qt::CaseInsensitive);
        report("the old hard-coded colours are gone", !literal);
    }
    tm.setActiveTheme(before);
}

void testLinksAreCanonCyan()
{
    QTemporaryFile file;
    if (!file.open()) {
        report("temp help document created", false);
        return;
    }
    file.write("See [the docs](https://docs.aethersdr.com) for more.\n");
    file.close();
    auto& tm = AetherSDR::ThemeManager::instance();
    const QString before = tm.activeTheme();
    tm.setActiveTheme(QStringLiteral("Default Dark"));
    HelpDialog dialog("Links", file.fileName());
    dialog.show();
    report("link guide is exposed", QTest::qWaitForWindowExposed(&dialog));
    auto* browser = dialog.findChild<QTextBrowser*>("helpBrowser");
    QColor previousCyan;
    for (const char* theme : {"Default Dark", "Default Light", "Default Dark"}) {
        report("switch theme while the guide stays open", tm.setActiveTheme(QLatin1String(theme)));
        const QColor cyan = tm.color(browser, QStringLiteral("color.canon.cyan"));
        if (previousCyan.isValid()) {
            report("the theme switch changes the expected link colour", cyan != previousCyan);
        }
        int links = 0;
        int cyanLinks = 0;
        for (QTextBlock b = browser->document()->begin(); b.isValid(); b = b.next()) {
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                if (it.fragment().charFormat().isAnchor()) {
                    ++links;
                    cyanLinks += it.fragment().charFormat().foreground().color() == cyan;
                }
            }
        }
        report("open guide links follow this theme's canon cyan", links > 0 && cyanLinks == links);
        previousCyan = cyan;
    }
    tm.setActiveTheme(before);
}

void testReopenMinimizedGuide()
{
    QTemporaryFile file;
    if (!writeTempHelp(file)) {
        report("temp help document created", false);
        return;
    }
    HelpDialog dialog("Reopen Test", file.fileName());
    dialog.showAndActivate();
    report("reopened guide is exposed", QTest::qWaitForWindowExposed(&dialog));
    for (const Qt::WindowStates retained : {Qt::WindowStates(),
                                          Qt::WindowStates(Qt::WindowMaximized),
                                          Qt::WindowStates(Qt::WindowFullScreen)}) {
        dialog.setWindowState(retained | Qt::WindowMinimized);
        report("guide is minimized before reopening", dialog.isMinimized());
        dialog.showAndActivate();
        report("reopening clears the minimized state", !dialog.isMinimized());
        report("reopening preserves maximized/fullscreen state",
               (dialog.windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen)) == retained);
        report("reopened guide stays visible", dialog.isVisible());
    }
}

void testClosePaths()
{
    QTemporaryFile file;
    if (!writeTempHelp(file)) { report("temp help document created", false); return; }
    for (int path = 0; path < 3; ++path) {
        QPointer<HelpDialog> d = new HelpDialog("Close Test", file.fileName());
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->show();
        const bool exposed = QTest::qWaitForWindowExposed(d);
        if (path == 0) QTest::keyClick(d, Qt::Key_Escape);
        if (path == 1) {
            d->activateWindow();
            QTest::qWaitForWindowActive(d);
            QTest::keySequence(d, QKeySequence::Close);
        }
        if (path == 2) {
            QToolButton* corner = nullptr;
            for (QToolButton* b : d->findChildren<QToolButton*>())
                if (b->accessibleName() == QLatin1String("Close")) corner = b;
            report("the corner close button exists", corner != nullptr);
            if (corner) QTest::mouseClick(corner, Qt::LeftButton);
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        report(path == 0 ? "Esc destroys the guide"
               : path == 1 ? "Ctrl+W / Cmd+W destroys the guide"
                           : "the corner close button destroys the guide",
               exposed && d.isNull());
        delete d.data();
    }
}

void testLegibleAtMinimum()
{
    QTemporaryFile file;
    if (!writeTempHelp(file)) { report("temp help document created", false); return; }
    auto& tm = AetherSDR::ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        tm.setActiveTheme(QLatin1String(theme));
        HelpDialog d("Floor Test", file.fileName());
        d.show();
        QTest::qWaitForWindowExposed(&d);
        d.resize(QSize(1, 1));
        QTest::qWait(50);
        bool legible = true;
        for (QWidget* w : d.bodyWidget()->findChildren<QWidget*>()) {
            if (!w->isVisible()) continue;
            bool scrolled = false;
            for (QWidget* p = w->parentWidget(); p && p != &d; p = p->parentWidget())
                scrolled = scrolled || qobject_cast<QAbstractScrollArea*>(p) != nullptr;
            if (scrolled) continue;
            int need = w->minimumSizeHint().height();
            if (auto* l = qobject_cast<QLabel*>(w); l && l->wordWrap()) need = l->heightForWidth(l->width());
            if (w->height() < need) {
                legible = false;
                std::printf("  %s: %s h=%d need=%d\n", theme, w->metaObject()->className(), w->height(), need);
            }
        }
        std::printf("  %s: shown minimum %dx%d\n", theme, d.width(), d.height());
        report("every control keeps its height at the smallest size", legible);
        report("the shown minimum height fits a 768 px screen", d.height() <= 700);
        d.hide();
    }
    tm.setActiveTheme(before);
}

void testGeometryCarriesOver()
{
    QTemporaryFile file(QDir::tempPath() + QStringLiteral("/canon-geometry-XXXXXX.md"));
    if (!writeTempHelp(file)) { report("temp help document created", false); return; }
    const QString key = QStringLiteral("HelpDialogGeometry_") + QFileInfo(file.fileName()).baseName();
    auto& settings = AppSettings::instance();
    settings.remove(key);
    HelpDialog restored("Geometry Test", file.fileName());
    const QSize chosen(restored.minimumWidth() + 30, restored.minimumHeight() + 40);
    QDialog legacy;
    legacy.resize(chosen);
    settings.setValue(key, QString::fromLatin1(legacy.saveGeometry().toBase64()));
    restored.show();
    QTest::qWaitForWindowExposed(&restored);
    for (int i = 0; i < 50 && restored.size() != chosen; ++i)
        QTest::qWait(20);
    report("geometry saved under the guide's old key restores", restored.size() == chosen);
    restored.hide();
}


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
    testCanonWindow();
    testLinksAreCanonCyan();
    testReopenMinimizedGuide();
    testClosePaths();
    testLegibleAtMinimum();
    testGeometryCarriesOver();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}

#include "help_dialog_test.moc"
