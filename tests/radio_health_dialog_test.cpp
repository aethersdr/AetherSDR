// Radio Health on the style guide's CanonWindow (RFC #6226, #7027): a
// frameless workspace window in the canon scope that keeps the old dialog's
// geometry key and polls the radio only while it is shown. No radio model: the
// window is built with none, so refresh() reads nothing.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/CanonWindow.h"
#include "gui/RadioHealthDialog.h"

#include <QApplication>
#include <QDialog>
#include <QKeySequence>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTest>
#include <QToolButton>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* name)
{
    std::printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
    g_failures += !ok;
}

void canonWorkspace()
{
    RadioHealthDialog dialog(nullptr);
    check(qobject_cast<CanonWindow*>(&dialog) != nullptr, "Radio Health is a CanonWindow");
    check(dialog.windowType() == Qt::Window
              && dialog.windowFlags().testFlag(Qt::FramelessWindowHint),
          "it is a frameless workspace window, not a dialog on its parent");
    check(dialog.findChild<QLabel*>(QStringLiteral("canonHeaderTitle")) != nullptr,
          "it carries the canon header");
    check(dialog.windowTitle() == QLatin1String("Radio Health"), "the title is unchanged");
    check(dialog.objectName() == QLatin1String("RadioHealthDialog"), "the window keeps its name");
    check(dialog.findChild<QTableWidget*>(QStringLiteral("radioHealthTable")) != nullptr,
          "the register table is named");
    check(dialog.findChild<QPushButton*>(QStringLiteral("radioHealthCopy")) != nullptr,
          "Copy is named");
    bool closeButton = false;
    for (auto* b : dialog.findChildren<QPushButton*>())
        closeButton |= b->text() == QLatin1String("Close");
    check(!closeButton, "the footer Close button is gone: the canon close paths replace it");
}

// The table's tokens resolve through canon/radioHealth to the canon palette,
// not the root theme (an unregistered scope would silently fall back to root).
void tableFollowsTheCanonScope()
{
    RadioHealthDialog dialog(nullptr);
    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("radioHealthTable"));
    if (!table) return;
    auto& tm = ThemeManager::instance();
    QWidget outside;
    const QColor inside = tm.color(table, QStringLiteral("color.background.1"));
    const QColor root = tm.color(&outside, QStringLiteral("color.background.1"));
    check(inside.isValid() && inside != root,
          "the table resolves base tokens through the canon scope, not the root theme");
    check(table->styleSheet().isEmpty(),
          "no per-widget sheet on the table overrides the canon body sheet");
}

// The 500 ms poll runs only while the window is shown: Esc and Ctrl+W close
// through QDialog::done(), which hides without a close event.
void pollsOnlyWhileShown()
{
    RadioHealthDialog dialog(nullptr);
    check(!dialog.isRefreshing(), "a window not yet shown does not poll");
    dialog.show();
    check(QTest::qWaitForWindowExposed(&dialog), "the window is exposed");
    check(dialog.isRefreshing(), "the shown window polls");
    QTest::keyClick(&dialog, Qt::Key_Escape);
    check(!dialog.isRefreshing(), "Esc stops the poll");
    dialog.show();
    check(dialog.isRefreshing(), "reopening resumes the poll");
    dialog.hide();
}

// MainWindow opens it through showOrRaiseCanon() with WA_DeleteOnClose, so
// every way out destroys it and the next open is a fresh window.
void everyCloseDestroysTheWindow()
{
    for (int path = 0; path < 3; ++path) {
        QPointer<RadioHealthDialog> dialog = new RadioHealthDialog(nullptr);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
        const bool exposed = QTest::qWaitForWindowExposed(dialog);
        if (path == 0) QTest::keyClick(dialog, Qt::Key_Escape);
        if (path == 1) {
            // A window shortcut: it needs the window active, which a desktop
            // gives a newly shown window but a background test process may not.
            dialog->activateWindow();
            QTest::qWaitForWindowActive(dialog);
            QTest::keySequence(dialog, QKeySequence::Close);
        }
        if (path == 2) {
            // Press the corner ✕ itself, so its connection is pinned too.
            QToolButton* corner = nullptr;
            for (QToolButton* b : dialog->findChildren<QToolButton*>())
                if (b->accessibleName() == QLatin1String("Close")) corner = b;
            check(corner != nullptr, "the corner close button exists");
            if (corner) QTest::mouseClick(corner, Qt::LeftButton);
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(exposed && dialog.isNull(),
              path == 0 ? "Esc destroys the window"
              : path == 1 ? "Ctrl+W / Cmd+W destroys the window"
                          : "the corner close button destroys the window");
        delete dialog.data();
    }
}

// Return on the focused table must not click anything: a workspace never
// clicks a default button, and Copy is not an auto-default.
void returnClicksNothing()
{
    RadioHealthDialog dialog(nullptr);
    dialog.show();
    check(QTest::qWaitForWindowExposed(&dialog), "the window is exposed");
    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("radioHealthTable"));
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("radioHealthStatus"));
    if (!table || !status) return;
    table->setFocus();
    const QString before = status->text();
    QTest::keyClick(table, Qt::Key_Return);
    check(dialog.isVisible() && status->text() == before,
          "Return in the table neither copies nor closes the window");
    dialog.hide();
}

// At the smallest size the shown window allows, every control keeps the
// height it needs (the wrapped intro: its height for its width), and that size
// fits a 1366x768 screen. Measured on the shown, polished window, in both
// themes: the constructor's hint comes before the canon sheet.
void minimumSizeFits()
{
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        tm.setActiveTheme(QLatin1String(theme));
        RadioHealthDialog dialog(nullptr);
        dialog.show();
        QTest::qWaitForWindowExposed(&dialog);
        dialog.resize(QSize(1, 1));
        QTest::qWait(50);
        bool legible = true;
        for (QWidget* w : dialog.bodyWidget()->findChildren<QWidget*>()) {
            if (!w->isVisible() || qobject_cast<QTableWidget*>(w->parentWidget())) continue;
            int need = w->minimumSizeHint().height();
            if (auto* label = qobject_cast<QLabel*>(w); label && label->wordWrap())
                need = label->heightForWidth(label->width());
            if (w->height() < need) {
                legible = false;
                std::printf("  %s: %s h=%d need=%d\n", theme, w->metaObject()->className(),
                            w->height(), need);
            }
        }
        std::printf("  %s: shown minimum %dx%d\n", theme, dialog.width(), dialog.height());
        check(legible, "every control keeps its height at the smallest size");
        check(dialog.height() <= 700, "the minimum height fits a 768 px screen");
        dialog.hide();
    }
    tm.setActiveTheme(before);
}

// Size and position persist under RadioHealthDialogGeometry, the key the old
// dialog wrote, including its base64 rectangles.
void geometryCarriesOver()
{
    static const QString kKey = QStringLiteral("RadioHealthDialogGeometry");
    auto& settings = AppSettings::instance();
    settings.remove(kKey);

    RadioHealthDialog restored(nullptr);
    const QSize chosen(restored.minimumWidth() + 30, restored.minimumHeight() + 20);
    QDialog legacy;
    legacy.resize(chosen);
    settings.setValue(kKey, QString::fromLatin1(legacy.saveGeometry().toBase64()));
    check(restored.size() != chosen, "control: a fresh window is not born at the saved size");
    restored.show();
    check(QTest::qWaitForWindowExposed(&restored), "the window is exposed");
    for (int i = 0; i < 50 && restored.size() != chosen; ++i)
        QTest::qWait(20);
    check(restored.size() == chosen, "geometry saved by the old dialog restores under its key");

    const QSize resized = chosen + QSize(20, 10);
    restored.resize(resized);
    QCoreApplication::processEvents();
    restored.hide();
    const QString saved = settings.value(kKey).toString();
    check(saved.endsWith(QStringLiteral(",%1,%2").arg(resized.width()).arg(resized.height())),
          "hiding the window saves its size under the same key");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("radio-health-dialog");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();

    canonWorkspace();
    tableFollowsTheCanonScope();
    pollsOnlyWhileShown();
    everyCloseDestroysTheWindow();
    returnClicksNothing();
    minimumSizeFits();
    geometryCarriesOver();

    return g_failures ? 1 : 0;
}
