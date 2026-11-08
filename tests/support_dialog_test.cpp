// Support & Diagnostics on the style guide's CanonWindow (RFC #6226, #7051):
// a short-lived canon dialog with no layout warnings, a plain-text log path,
// every close path, a legible floor and its old geometry key. No radio, no
// sockets: the window only reads the log file.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/LogManager.h"
#include "core/ThemeManager.h"
#include "gui/CanonWindow.h"
#include "gui/SupportDialog.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QFont>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QPoint>
#include <QPointer>
#include <QPushButton>
#include <QRect>
#include <QScrollArea>
#include <QScrollBar>
#include <QSize>
#include <QStringList>
#include <QTest>
#include <QToolButton>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;
QStringList g_layoutWarnings;
QtMessageHandler g_previous = nullptr;

void check(bool ok, const char* name)
{
    std::printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
    g_failures += !ok;
}

void captureLayoutWarnings(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (msg.contains(QLatin1String("already has a parent"))
        || msg.contains(QLatin1String("which already has a layout")))
        g_layoutWarnings << msg;
    if (g_previous) g_previous(type, ctx, msg);
}

void canonWindow()
{
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        check(tm.setActiveTheme(QLatin1String(theme)), theme);
        g_layoutWarnings.clear();
        SupportDialog d;
        check(g_layoutWarnings.isEmpty(), "building the window logs no layout warnings");
        for (const QString& w : g_layoutWarnings) std::printf("  warning: %s\n", qPrintable(w));
        check(qobject_cast<CanonWindow*>(&d) != nullptr, "Support & Diagnostics is a CanonWindow");
        check(d.objectName() == "SupportDialog", "it keeps the name screens.json waits for");
        check(d.property("themeContainer").toString() == "canon/support", "it uses the canon/support scope");
        auto* header = d.findChild<QLabel*>("canonHeaderTitle");
        check(header && header->text() == "Support & Diagnostics", "the canon header carries the title");
        bool closeButton = false;
        for (QPushButton* b : d.findChildren<QPushButton*>())
            closeButton = closeButton || b->text() == "Close";
        check(!closeButton, "the footer Close is gone");
        const QString sheet = d.bodyWidget()->styleSheet();
        check(!sheet.contains(": ;") && sheet.contains("QMenu") && sheet.contains("selection-color"),
              "the sheet resolves, with menu and selection rules");
        bool literal = false;
        for (QWidget* w : d.findChildren<QWidget*>())
            literal = literal || w->styleSheet().contains(QLatin1String("#8898a8"), Qt::CaseInsensitive);
        check(!literal, "the old hard-coded colours are gone");
        check(d.findChildren<QCheckBox*>().size() == LogManager::instance().categories().size(),
              "one canon check box per log category");
        auto* path = d.findChild<QLineEdit*>("supportLogPath");
        check(path && path->isReadOnly() && !path->text().isEmpty() && path->toolTip() == path->text(),
              "the log path is a read-only field: whole, selectable and copyable");
    }
    tm.setActiveTheme(before);
}

void closePaths()
{
    for (int path = 0; path < 3; ++path) {
        QPointer<SupportDialog> d = new SupportDialog;
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

// At the smallest size the shown window allows, every control outside the
// log pane keeps its height (the buttons lost their fixed 24-26 px heights,
// which the canon padding would have clipped).
void legibleAtMinimum()
{
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        tm.setActiveTheme(QLatin1String(theme));
        SupportDialog d;
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
                std::printf("  %s: %s %s h=%d need=%d\n", theme, w->metaObject()->className(),
                            qPrintable(w->objectName()), w->height(), need);
            }
        }
        std::printf("  %s: shown minimum %dx%d\n", theme, d.width(), d.height());
        check(legible, "every control keeps its height at the smallest size");
        check(d.height() <= 700, "the shown minimum height fits a 768 px screen");
        d.hide();
    }
    tm.setActiveTheme(before);
}

// The category page may be taller than its viewport, but each complete
// checkbox must fit horizontally and be reachable by vertical scrolling.
void categoriesRemainLegible()
{
    ThemeManager& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        tm.setActiveTheme(QLatin1String(theme));
        SupportDialog dialog;
        dialog.show();
        check(QTest::qWaitForWindowExposed(&dialog), "category window is exposed");
        dialog.resize(QSize(1, 1));
        QTest::qWait(50);

        auto* scroll = dialog.findChild<QScrollArea*>("supportCategoryScroll");
        check(scroll && scroll->widget(), "categories have a scrollable page");
        if (!scroll || !scroll->widget()) {
            continue;
        }
        const QList<QCheckBox*> boxes = scroll->widget()->findChildren<QCheckBox*>();
        check(!boxes.isEmpty()
                  && boxes.size() == LogManager::instance().categories().size(),
              "every category is on the scrolling page");

        bool fullSize = true;
        bool reachable = true;
        for (QCheckBox* box : boxes) {
            const QSize need = box->sizeHint();
            if (box->width() < need.width() || box->height() < need.height()) {
                fullSize = false;
                std::printf("  %s: %s size=%dx%d need=%dx%d\n", theme,
                            qPrintable(box->text()), box->width(), box->height(),
                            need.width(), need.height());
            }
            scroll->ensureWidgetVisible(box, 0, 0);
            QCoreApplication::processEvents();
            const QRect visibleBox(box->mapTo(scroll->viewport(), QPoint(0, 0)),
                                   box->size());
            if (!box->isVisible() || !scroll->viewport()->rect().contains(visibleBox)) {
                reachable = false;
                std::printf("  %s: %s is clipped in the viewport\n",
                            theme, qPrintable(box->text()));
            }
        }
        check(fullSize, "all category labels and indicators keep their full width and height");
        check(reachable, "every category, including the first and last rows, scrolls fully into view");
        if (scroll->horizontalScrollBar()->maximum() != 0) {
            std::printf("  %s: scroll=%d minimum=%d frame=%d viewport=%d page=%d pageMin=%d bar=%d range=%d\n",
                        theme, scroll->width(), scroll->minimumWidth(), scroll->frameWidth(),
                        scroll->viewport()->width(), scroll->widget()->width(),
                        scroll->widget()->minimumSizeHint().width(),
                        scroll->verticalScrollBar()->sizeHint().width(),
                        scroll->horizontalScrollBar()->maximum());
        }
        check(scroll->horizontalScrollBar()->maximum() == 0,
              "all category columns fit without horizontal scrolling");
        dialog.hide();
    }
    tm.setActiveTheme(before);
}

void geometryCarriesOver()
{
    static const QString kKey = QStringLiteral("SupportDialogGeometry");
    auto& settings = AppSettings::instance();
    settings.remove(kKey);
    SupportDialog restored;
    const QSize chosen(restored.minimumWidth() + 30, restored.minimumHeight() + 20);
    QDialog legacy;
    legacy.resize(chosen);
    settings.setValue(kKey, QString::fromLatin1(legacy.saveGeometry().toBase64()));
    restored.show();
    QTest::qWaitForWindowExposed(&restored);
    for (int i = 0; i < 50 && restored.size() != chosen; ++i)
        QTest::qWait(20);
    check(restored.size() == chosen, "geometry saved by the old dialog restores under its key");
    restored.hide();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("support-dialog");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    g_previous = qInstallMessageHandler(captureLayoutWarnings);

    canonWindow();
    closePaths();
    legibleAtMinimum();
    categoriesRemainLegible();
    // The category grid inherits a pixel-sized canon font. Small, wide platform
    // font metrics expose measurements made before that inherited style is polished.
    const QFont originalFont = app.font();
    QFont smallFont = originalFont;
    smallFont.setPointSize(8);
    smallFont.setStretch(150);
    app.setFont(smallFont);
    categoriesRemainLegible();
    app.setFont(originalFont);
    geometryCarriesOver();

    qInstallMessageHandler(g_previous);
    return g_failures ? 1 : 0;
}
