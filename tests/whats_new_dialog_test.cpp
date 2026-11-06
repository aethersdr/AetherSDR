// What's New on the style guide's CanonWindow (RFC #6226, #7045): a short-lived
// canon dialog whose release notes take canon colours in each theme, whose Find
// is a canon prompt, and whose close paths destroy it. Socket-free: the fetch
// from GitHub is switched off and notes are injected.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/CanonWindow.h"
#include "gui/WhatsNewDialog.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QColor>
#include <QDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QTextBlock>
#include <QTextFragment>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QTest>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <cstdio>
#include <functional>

using namespace AetherSDR;

namespace AetherSDR {
class WhatsNewDialogTestAccess {
public:
    static void noFetch() { WhatsNewDialog::s_fetchReleaseNotes = false; }
    static void notes(WhatsNewDialog& d, const QString& md)
    {
        d.showLiveReleaseNotes(QStringLiteral("AetherSDR v1.2.3"), QStringLiteral("v1.2.3"),
                               QString(), md);
    }
    static void error(WhatsNewDialog& d, const QString& m) { d.showReleaseLoadError(m); }
};
} // namespace AetherSDR

namespace {

int g_failures = 0;

void check(bool ok, const char* name)
{
    std::printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
    g_failures += !ok;
}

WhatsNewDialog* make()
{
    return new WhatsNewDialog(QString(), QStringLiteral("1.2.3"), nullptr, false, true);
}

void canonWindow()
{
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    QString darkInk;
    for (const char* theme : {"Default Dark", "Default Light"}) {
        check(tm.setActiveTheme(QLatin1String(theme)), theme);
        QPointer<WhatsNewDialog> d = make();
        check(qobject_cast<CanonWindow*>(d.data()) != nullptr, "What's New is a CanonWindow");
        check(d->objectName() == "WhatsNewDialog", "it is named for the bridge and screenshots");
        check(d->property("themeContainer").toString() == "canon/whatsNew", "it uses the canon/whatsNew scope");
        auto* header = d->findChild<QLabel*>("canonHeaderTitle");
        check(header && header->text() == "What's New", "the canon header says What's New");
        bool closeButton = false;
        for (QPushButton* b : d->findChildren<QPushButton*>())
            closeButton = closeButton || b->text() == "Close";
        check(!closeButton, "the footer Close is gone; the canon close paths replace it");
        const QString sheet = d->bodyWidget()->styleSheet();
        check(!sheet.contains(": ;") && sheet.contains("QMenu") && sheet.contains("selection-color"),
              "the sheet resolves, and gives menus a panel and selections an ink");

        // The notes are rich text: their colours come from this theme's canon
        // tokens, not the old fixed dark palette.
        WhatsNewDialogTestAccess::notes(*d, QStringLiteral(
            "# Heading\n\nSome **text** and [a link](https://example.com), "
            "[a **bold** link](https://example.com/b) and [another](https://example.com/c)."));
        auto* browser = d->findChild<QTextBrowser*>("whatsNewNotes");
        const QString docSheet = browser ? browser->document()->defaultStyleSheet() : QString();
        const QString ink = tm.color(d, QStringLiteral("color.canon.ink")).name();
        check(docSheet.contains(ink) && !docSheet.contains("#d6e2ee") && !docSheet.contains("#f2f7fb"),
              "the notes' stylesheet takes this theme's canon ink, not the old fixed colours");
        // The markdown importer bakes a link colour into each link's format,
        // and that is what is drawn: it must be this theme's canon cyan.
        const QColor cyan = tm.color(d, QStringLiteral("color.canon.cyan"));
        int links = 0, cyanLinks = 0;
        for (QTextBlock b = browser->document()->begin(); b.isValid(); b = b.next())
            for (auto it = b.begin(); !it.atEnd(); ++it)
                if (it.fragment().charFormat().isAnchor()) {
                    ++links;
                    cyanLinks += it.fragment().charFormat().foreground().color() == cyan;
                }
        check(links > 0 && cyanLinks == links, "links in the notes are drawn in this theme's canon cyan");
        if (QLatin1String(theme) == QLatin1String("Default Dark")) darkInk = ink;
        else check(ink != darkInk, "control: the canon ink differs between the themes");
        delete d.data();
    }

    // A theme change while the window is open re-renders the notes.
    tm.setActiveTheme(QStringLiteral("Default Dark"));
    QPointer<WhatsNewDialog> d = make();
    WhatsNewDialogTestAccess::notes(*d, QStringLiteral("# Heading\n\nBody."));
    tm.setActiveTheme(QStringLiteral("Default Light"));
    QCoreApplication::processEvents();
    auto* browser = d->findChild<QTextBrowser*>("whatsNewNotes");
    check(browser && browser->document()->defaultStyleSheet()
                         .contains(tm.color(d, QStringLiteral("color.canon.ink")).name()),
          "switching theme while open re-renders the notes in the new colours");
    WhatsNewDialogTestAccess::error(*d, QStringLiteral("offline <b>"));
    check(browser && browser->toPlainText().contains("offline <b>"),
          "the load error shows its message as text, not markup");
    delete d.data();
    tm.setActiveTheme(before);
}

// The upgrade path, which no menu reaches today, keeps its buttons and its
// welcome heading: Upgrade and Skip this version appear only with showUpgrade,
// and an unseen version without currentVersionOnly says Welcome!.
void upgradePath()
{
    const auto hasButton = [](WhatsNewDialog* d, const char* text) {
        for (QPushButton* b : d->findChildren<QPushButton*>())
            if (b->text() == QLatin1String(text)) return true;
        return false;
    };
    QPointer<WhatsNewDialog> help = make();
    check(!hasButton(help, "Upgrade") && !hasButton(help, "Skip this version"),
          "Help -> What's New shows no Upgrade or Skip this version");
    delete help.data();

    QPointer<WhatsNewDialog> upgrade =
        new WhatsNewDialog(QString(), QStringLiteral("1.2.3"), nullptr, true, false);
    check(hasButton(upgrade, "Upgrade") && hasButton(upgrade, "Skip this version"),
          "the upgrade path keeps Upgrade and Skip this version");
    auto* header = upgrade->findChild<QLabel*>("canonHeaderTitle");
    check(header && header->text() == "Welcome!", "an unseen version is welcomed");
    delete upgrade.data();
}

// A theme change while the notes are scrolled keeps the reader's place.
void themeChangeKeepsScroll()
{
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    tm.setActiveTheme(QStringLiteral("Default Dark"));
    QPointer<WhatsNewDialog> d = make();
    d->show();
    QTest::qWaitForWindowExposed(d);
    QString md;
    for (int i = 0; i < 400; ++i)
        md += QStringLiteral("## Section %1\n\nA line of release notes, number %1.\n\n").arg(i);
    WhatsNewDialogTestAccess::notes(*d, md);
    auto* browser = d->findChild<QTextBrowser*>("whatsNewNotes");
    QScrollBar* bar = browser ? browser->verticalScrollBar() : nullptr;
    if (!bar) { check(false, "the notes scroll"); delete d.data(); return; }
    QCoreApplication::processEvents();
    const int target = bar->maximum() * 3 / 4;
    bar->setValue(target);
    check(target > 0 && bar->value() == target, "control: the long notes scroll deep");
    tm.setActiveTheme(QStringLiteral("Default Light"));
    check(bar->value() == target, "a theme change keeps the notes' scroll position");
    QCoreApplication::processEvents();
    check(bar->value() == target, "the scroll position holds once events settle");
    delete d.data();
    tm.setActiveTheme(before);
}

// Find is a canon prompt with Find the default: Cancel and Esc search for
// nothing; Find with text moves the selection to a match.
void findPrompt()
{
    QPointer<WhatsNewDialog> d = make();
    d->show();
    QTest::qWaitForWindowExposed(d);
    WhatsNewDialogTestAccess::notes(*d, QStringLiteral("alpha\n\nbravo charlie\n\ndelta"));
    auto* browser = d->findChild<QTextBrowser*>("whatsNewNotes");
    auto* find = d->findChild<QPushButton*>("whatsNewFind");
    if (!browser || !find) { check(false, "the notes and Find exist"); return; }

    const auto run = [&](std::function<void(CanonWindow*)> answer) {
        QString seenDefault;
        bool canon = false, seen = false;
        QTimer driver;
        QObject::connect(&driver, &QTimer::timeout, [&] {
            if (seen) return;
            for (QWidget* w : QApplication::topLevelWidgets()) {
                auto* p = qobject_cast<CanonWindow*>(w);
                if (!p || !p->isVisible() || p->objectName() != "whatsNewFindPrompt") continue;
                seen = true;
                canon = p->findChild<QLabel*>("canonHeaderTitle") != nullptr;
                for (QPushButton* b : p->findChildren<QPushButton*>())
                    if (b->isDefault()) seenDefault = b->text();
                answer(p);
                return;
            }
        });
        driver.start(20);
        QTimer::singleShot(10000, [] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* dd = qobject_cast<QDialog*>(w); dd && dd->isModal() && dd->isVisible()) dd->reject();
        });
        browser->moveCursor(QTextCursor::Start);
        find->click();
        QTest::qWait(30);
        return std::make_tuple(seen, canon, seenDefault);
    };

    auto [seen, canon, def] = run([](CanonWindow* p) {
        if (auto* f = p->findChild<QLineEdit*>("whatsNewFindField")) f->setText("charlie");
        for (QPushButton* b : p->findChildren<QPushButton*>()) if (b->text() == "Cancel") b->click();
    });
    check(seen && canon, "Find opens a canon prompt");
    check(def == "Find", "Find is the prompt's default");
    check(!browser->textCursor().hasSelection(), "Cancel searches for nothing");
    run([](CanonWindow* p) {
        if (auto* f = p->findChild<QLineEdit*>("whatsNewFindField")) f->setText("charlie");
        QTest::keyClick(p, Qt::Key_Escape);
    });
    check(!browser->textCursor().hasSelection(), "Esc searches for nothing");
    run([](CanonWindow* p) {
        auto* f = p->findChild<QLineEdit*>("whatsNewFindField");
        if (f) { f->setText("charlie"); QTest::keyClick(f, Qt::Key_Return); }
    });
    check(browser->textCursor().selectedText() == "charlie", "Return finds the text");
    delete d.data();
}

// Every way out destroys the window (it is WA_DeleteOnClose).
void closePaths()
{
    for (int path = 0; path < 3; ++path) {
        QPointer<WhatsNewDialog> d = make();
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
            check(corner != nullptr, "the corner close button exists");
            if (corner) QTest::mouseClick(corner, Qt::LeftButton);
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(exposed && d.isNull(),
              path == 0 ? "Esc destroys the window"
              : path == 1 ? "Ctrl+W / Cmd+W destroys the window"
                          : "the corner close button destroys the window");
        delete d.data();
    }
}

// At the smallest size the shown window allows, every control outside the
// notes keeps its height; the floor fits a 768 px screen.
void legibleAtMinimum()
{
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        tm.setActiveTheme(QLatin1String(theme));
        QPointer<WhatsNewDialog> d = make();
        d->show();
        QTest::qWaitForWindowExposed(d);
        d->resize(QSize(1, 1));
        QTest::qWait(50);
        bool legible = true;
        for (QWidget* w : d->bodyWidget()->findChildren<QWidget*>()) {
            if (!w->isVisible()) continue;
            bool scrolled = false;
            for (QWidget* p = w->parentWidget(); p && p != d; p = p->parentWidget())
                scrolled = scrolled || qobject_cast<QAbstractScrollArea*>(p) != nullptr;
            if (scrolled) continue;
            int need = w->minimumSizeHint().height();
            if (auto* l = qobject_cast<QLabel*>(w); l && l->wordWrap()) need = l->heightForWidth(l->width());
            if (w->height() < need) {
                legible = false;
                std::printf("  %s: %s h=%d need=%d\n", theme, w->metaObject()->className(), w->height(), need);
            }
        }
        std::printf("  %s: shown minimum %dx%d\n", theme, d->width(), d->height());
        check(legible, "every control keeps its height at the smallest size");
        check(d->height() <= 700, "the shown minimum height fits a 768 px screen");
        delete d.data();
    }
    tm.setActiveTheme(before);
}

// Size and position persist under WhatsNewDialogGeometry, the key the old
// dialog wrote, including its base64 rectangles.
void geometryCarriesOver()
{
    static const QString kKey = QStringLiteral("WhatsNewDialogGeometry");
    auto& settings = AppSettings::instance();
    settings.remove(kKey);
    QPointer<WhatsNewDialog> restored = make();
    const QSize chosen(restored->minimumWidth() + 30, restored->minimumHeight() + 40);
    QDialog legacy;
    legacy.resize(chosen);
    settings.setValue(kKey, QString::fromLatin1(legacy.saveGeometry().toBase64()));
    restored->show();
    QTest::qWaitForWindowExposed(restored);
    for (int i = 0; i < 50 && restored->size() != chosen; ++i)
        QTest::qWait(20);
    check(restored->size() == chosen, "geometry saved by the old dialog restores under its key");
    delete restored.data();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("whats-new-dialog");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    WhatsNewDialogTestAccess::noFetch();

    canonWindow();
    upgradePath();
    themeChangeKeepsScroll();
    findPrompt();
    closePaths();
    legibleAtMinimum();
    geometryCarriesOver();

    return g_failures ? 1 : 0;
}
