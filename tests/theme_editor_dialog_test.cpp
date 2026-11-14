// Theme Editor on the style guide's CanonWindow (RFC #6226, #7057): canon
// chrome, while every token row still shows the theme being edited, not the
// canon scope's re-map; canon name prompts; every close path; a measured
// floor; its old geometry key.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/CanonWindow.h"
#include "gui/ThemeEditorDialog.h"
#include "gui/ThemeInspector.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QWidget>
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

QString hexOf(const QColor& c)
{
    return c.alpha() == 255 ? c.name(QColor::HexRgb) : c.name(QColor::HexArgb);
}

void canonWindow()
{
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        check(tm.setActiveTheme(QLatin1String(theme)), theme);
        ThemeEditorDialog d;
        check(qobject_cast<CanonWindow*>(&d) != nullptr, "Theme Editor is a CanonWindow");
        check(d.windowType() == Qt::Window && d.windowFlags().testFlag(Qt::FramelessWindowHint),
              "it is a frameless workspace window");
        check(d.objectName() == "ThemeEditorDialog", "it keeps the name screens.json waits for");
        check(d.property("themeContainer").toString() == "canon/themeEditor",
              "it uses the canon/themeEditor scope");
        auto* header = d.findChild<QLabel*>("canonHeaderTitle");
        check(header && header->text() == "Theme Editor", "the canon header carries the title");
        bool closeButton = false;
        for (QPushButton* b : d.findChildren<QPushButton*>())
            closeButton = closeButton || b->text() == "Close";
        check(!closeButton, "the footer Close is gone");
        const QString sheet = d.bodyWidget()->styleSheet();
        check(!sheet.contains(": ;") && sheet.contains("QMenu") && sheet.contains("selection-color"),
              "the sheet resolves, with menu and selection rules");

        // The preview must stay the edited theme. color.background.0 is one of
        // the tokens the canon scope re-maps, so the editor's own widgets
        // resolve it differently from the root theme; its row must show the
        // root (edited) value, not the canon one.
        const QString key = QStringLiteral("color.background.0");
        const QColor rootValue = tm.color(key);
        const QColor canonValue = tm.color(d.bodyWidget(), key);
        check(rootValue != canonValue, "control: the canon scope re-maps color.background.0");
        auto* tree = d.findChild<QTreeWidget*>();
        QTreeWidgetItem* row = nullptr;
        for (QTreeWidgetItemIterator it(tree); tree && *it; ++it)
            if ((*it)->data(0, Qt::UserRole).toString() == key) row = *it;
        const int last = tree ? tree->columnCount() - 1 : 0;
        check(row && row->text(last).compare(hexOf(rootValue), Qt::CaseInsensitive) == 0,
              "the token row shows the edited theme's value, not the canon re-map");
        if (row) std::printf("  row %s, root %s, canon %s\n", qPrintable(row->text(last)),
                             qPrintable(hexOf(rootValue)), qPrintable(hexOf(canonValue)));
    }
    tm.setActiveTheme(before);
}

// Save As on a built-in theme opens a canon name prompt with Save the default;
// Esc and Cancel save nothing.
void namePromptIsCanon()
{
    auto& tm = ThemeManager::instance();
    tm.setActiveTheme(QStringLiteral("Default Dark"));
    const QStringList themesBefore = tm.availableThemes();
    ThemeEditorDialog d;
    d.show();
    QTest::qWaitForWindowExposed(&d);
    QPushButton* saveAs = nullptr;
    for (QPushButton* b : d.findChildren<QPushButton*>())
        if (b->text().contains(QLatin1String("Save As"), Qt::CaseInsensitive)) saveAs = b;
    if (!saveAs) { check(false, "Save As exists"); return; }

    for (const auto& [label, answer] : std::initializer_list<std::pair<const char*, std::function<void(CanonWindow*)>>>{
             {"Esc on the name prompt saves nothing",
              [](CanonWindow* w) { QTest::keyClick(w, Qt::Key_Escape); }},
             {"Cancel on the name prompt saves nothing",
              [](CanonWindow* w) { for (QPushButton* b : w->findChildren<QPushButton*>()) if (b->text() == "Cancel") b->click(); }}}) {
        bool seen = false, canon = false;
        QString def;
        QTimer driver;
        QObject::connect(&driver, &QTimer::timeout, [&] {
            if (seen) return;
            for (QWidget* w : QApplication::topLevelWidgets()) {
                auto* p = qobject_cast<CanonWindow*>(w);
                if (!p || !p->isVisible() || p->objectName() != "themeEditorNamePrompt") continue;
                seen = true;
                canon = p->findChild<QLabel*>("canonHeaderTitle") != nullptr;
                for (QPushButton* b : p->findChildren<QPushButton*>()) if (b->isDefault()) def = b->text();
                answer(p);
                return;
            }
        });
        driver.start(20);
        QTimer::singleShot(8000, [] { while (QWidget* m = QApplication::activeModalWidget()) m->close(); });
        saveAs->click();
        QTest::qWait(50);
        check(seen && canon && def == "Save", "Save As opens a canon name prompt, Save the default");
        check(tm.availableThemes() == themesBefore, label);
    }
    d.hide();
}

void closePaths()
{
    for (int path = 0; path < 3; ++path) {
        QPointer<ThemeEditorDialog> d = new ThemeEditorDialog;
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

// Every control outside the token list keeps its height at the floor. The
// floor itself is printed: the picker page needs most of a 768 px screen.
void legibleAtMinimum()
{
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        tm.setActiveTheme(QLatin1String(theme));
        ThemeEditorDialog d;
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
            if (scrolled || qobject_cast<QAbstractScrollArea*>(w)) continue;
            // Deliberately fixed-size controls (colour swatches, the eyedropper,
            // the B / I / U font buttons) draw their icon at that size.
            if (w->minimumHeight() == w->maximumHeight()) continue;
            int need = w->minimumSizeHint().height();
            if (auto* l = qobject_cast<QLabel*>(w); l && l->wordWrap()) need = l->heightForWidth(l->width());
            if (w->height() < need) {
                legible = false;
                std::printf("  %s: %s %s h=%d need=%d\n", theme, w->metaObject()->className(),
                            qPrintable(w->objectName()), w->height(), need);
            }
        }
        std::printf("  %s: shown minimum %dx%d\n", theme, d.width(), d.height());
        check(legible, "every control keeps its height at the smallest size");
        d.hide();
    }
    tm.setActiveTheme(before);
}

void geometryCarriesOver()
{
    static const QString kKey = QStringLiteral("ThemeEditorDialogGeometry");
    auto& settings = AppSettings::instance();
    settings.remove(kKey);
    ThemeEditorDialog restored;
    restored.show();
    QTest::qWaitForWindowExposed(&restored);
    const QSize floor = restored.minimumSize();
    restored.hide();
    const QSize chosen(floor.width() + 30, floor.height() + 10);
    QDialog legacy;
    legacy.resize(chosen);
    settings.setValue(kKey, QString::fromLatin1(legacy.saveGeometry().toBase64()));
    ThemeEditorDialog again;
    again.show();
    QTest::qWaitForWindowExposed(&again);
    for (int i = 0; i < 50 && again.size() != chosen; ++i)
        QTest::qWait(20);
    std::printf("  restored %dx%d, wanted %dx%d\n", again.width(), again.height(), chosen.width(), chosen.height());
    check(again.size() == chosen, "geometry saved by the old dialog restores under its key");
    again.hide();
}

// A pick click activates the main window; a Workspace is not kept above it,
// so the editor must come back to show the picked token.
void inspectorPickReturnsToEditor()
{
    QWidget mainWindow;
    mainWindow.resize(400, 300);
    mainWindow.show();
    ThemeEditorDialog d;
    d.show();
    const bool exposed = QTest::qWaitForWindowExposed(&d)
        && QTest::qWaitForWindowExposed(&mainWindow);
    mainWindow.activateWindow();
    const bool mainActive = QTest::qWaitForWindowActive(&mainWindow);
    auto* inspector = d.findChild<ThemeInspector*>();
    check(inspector != nullptr, "the editor owns an inspector");
    if (inspector) emit inspector->widgetPicked(&mainWindow, QPoint(10, 10));
    const bool editorActive = QTest::qWaitForWindowActive(&d);
    check(exposed && mainActive && editorActive, "a pick brings the editor back in front");
    d.hide();
    mainWindow.hide();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("theme-editor-dialog");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();

    canonWindow();
    namePromptIsCanon();
    closePaths();
    legibleAtMinimum();
    geometryCarriesOver();
    inspectorPickReturnsToEditor();

    return g_failures ? 1 : 0;
}
