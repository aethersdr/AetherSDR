// The shortcut editor on the style guide's CanonWindow (RFC #6226): a canon
// dialog in its own theme scope, whose questions are CanonMessageBoxes that
// change nothing unless their accepting button is clicked.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ShortcutManager.h"
#include "core/ThemeManager.h"
#include "gui/CanonMessageBox.h"
#include "gui/KeyboardMapWidget.h"
#include "gui/ShortcutDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QAbstractItemView>
#include <QImage>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QTableWidget>
#include <QPushButton>
#include <QTest>
#include <QTimer>
#include <cmath>
#include <cstdio>
#include <functional>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  expected (%s) to be true\n", \
                     __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

namespace {

const QString kPtt = QStringLiteral("ptt");
const QString kMute = QStringLiteral("mute");

// Answers the next CanonMessageBox to open, from inside its exec(). `seen`
// records whether one opened at all, and which button it opened on. Any other
// modal (a QMessageBox) is closed so the test fails rather than hangs.
struct Answer {
    bool opened{false};
    QString defaultText;
    QString title;
};

void answerNextBox(Answer& seen, std::function<void(CanonMessageBox&)> act)
{
    QTimer::singleShot(0, [&seen, act] {
        QWidget* modal = QApplication::activeModalWidget();
        auto* box = qobject_cast<CanonMessageBox*>(modal);
        if (!box) {
            if (modal) {
                modal->close();
            }
            return;
        }
        EXPECT_TRUE(QTest::qWaitForWindowExposed(box));
        seen.opened = true;
        seen.title = box->windowTitle();
        for (QPushButton* b : box->findChildren<QPushButton*>()) {
            if (b->isDefault()) {
                seen.defaultText = b->text();
            }
        }
        act(*box);
    });
}

// Counts the mouse presses that reach a window.
class PressSpy : public QObject {
public:
    int presses{0};

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::MouseButtonPress) {
            ++presses;
        }
        return false;
    }
};

QPushButton* buttonText(CanonMessageBox& box, const QString& text)
{
    for (QPushButton* b : box.findChildren<QPushButton*>()) {
        if (b->text() == text) {
            return b;
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-shortcut-dialog-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL could not create isolated settings profile\n");
        return 1;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    ThemeManager::instance();

    ShortcutManager mgr;
    // The keyboard map's first key, and the one Return selects when the map
    // takes the focus, is Esc.
    mgr.registerAction(kPtt, QStringLiteral("PTT"), QStringLiteral("TX"),
                       QKeySequence(Qt::Key_Escape), {});
    mgr.registerAction(kMute, QStringLiteral("Mute"), QStringLiteral("Audio"),
                       QKeySequence(), {});
    mgr.loadBindings();

    ShortcutDialog dlg(&mgr);
    dlg.show();
    EXPECT_TRUE(QTest::qWaitForWindowExposed(&dlg));

    // ---- a canon dialog in its own scope, with the canon header ----
    {
        EXPECT_TRUE(qobject_cast<CanonWindow*>(&dlg) != nullptr);
        EXPECT_TRUE(dlg.windowFlags().testFlag(Qt::FramelessWindowHint));
        EXPECT_TRUE(dlg.property("themeContainer").toString() == QStringLiteral("canon/shortcuts"));
        auto* heading = dlg.findChild<QLabel*>(QStringLiteral("canonHeaderTitle"));
        EXPECT_TRUE(heading && heading->text() == QStringLiteral("Keyboard Shortcuts"));
        EXPECT_TRUE(dlg.findChild<KeyboardMapWidget*>() != nullptr);
    }

    // ---- Reset All asks first, opens on Cancel, and Escape changes nothing ----
    auto* resetAll = dlg.findChild<QPushButton*>(QStringLiteral("shortcutResetAllButton"));
    EXPECT_TRUE(resetAll != nullptr);
    if (resetAll) {
        mgr.setBinding(kPtt, QKeySequence(Qt::Key_F5));
        Answer seen;
        answerNextBox(seen, [](CanonMessageBox& box) {
            QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &box,
                            Qt::Key_Escape);
        });
        resetAll->click();
        EXPECT_TRUE(seen.opened);
        EXPECT_TRUE(seen.title == QStringLiteral("Reset Shortcuts"));
        EXPECT_TRUE(seen.defaultText == QStringLiteral("Cancel"));
        EXPECT_TRUE(mgr.action(kPtt)->currentKey == QKeySequence(Qt::Key_F5));

        Answer again;
        answerNextBox(again, [](CanonMessageBox& box) {
            if (QPushButton* go = buttonText(box, QStringLiteral("Reset All"))) {
                go->click();
            }
        });
        resetAll->click();
        EXPECT_TRUE(again.opened);
        EXPECT_TRUE(mgr.action(kPtt)->currentKey == QKeySequence(Qt::Key_Escape));
    }

    // ---- a key already in use asks before reassigning; Cancel keeps it ----
    auto* map = dlg.findChild<KeyboardMapWidget*>();
    auto* combo = dlg.findChild<QComboBox*>();
    EXPECT_TRUE(map && combo);
    if (map && combo) {
        map->setFocus();
        QTest::keyClick(map, Qt::Key_Return);
        EXPECT_TRUE(map->selectedKey() == Qt::Key_Escape);
        // The first combo is the selected key's action.
        const int muteIdx = combo->findData(kMute);
        EXPECT_TRUE(muteIdx > 0);

        Answer seen;
        answerNextBox(seen, [](CanonMessageBox& box) {
            if (QPushButton* cancel = buttonText(box, QStringLiteral("Cancel"))) {
                cancel->click();
            }
        });
        combo->setCurrentIndex(muteIdx);   // what a user's pick in the list does
        emit combo->activated(muteIdx);
        EXPECT_TRUE(seen.opened);
        EXPECT_TRUE(seen.title == QStringLiteral("Key Conflict"));
        EXPECT_TRUE(seen.defaultText == QStringLiteral("Reassign"));
        EXPECT_TRUE(mgr.action(kPtt)->currentKey == QKeySequence(Qt::Key_Escape));
        EXPECT_TRUE(mgr.action(kMute)->currentKey.isEmpty());
        // ...and the Action field reads the key's own action again, not the
        // one just refused.
        EXPECT_TRUE(combo->currentData().toString() == kPtt);

        Answer again;
        answerNextBox(again, [](CanonMessageBox& box) {
            if (QPushButton* go = buttonText(box, QStringLiteral("Reassign"))) {
                go->click();
            }
        });
        emit combo->activated(muteIdx);
        EXPECT_TRUE(again.opened);
        EXPECT_TRUE(mgr.action(kMute)->currentKey == QKeySequence(Qt::Key_Escape));
        EXPECT_TRUE(mgr.action(kPtt)->currentKey.isEmpty());
    }

    // ---- Return in the Filter field presses no button: it used to press
    //      Clear and wipe the selected key's binding ----
    auto* filter = dlg.findChild<QLineEdit*>();
    EXPECT_TRUE(map && filter);
    if (map && filter) {
        mgr.resetToDefaults();
        map->setFocus();
        QTest::keyClick(map, Qt::Key_Return);   // selects Esc, bound to PTT
        EXPECT_TRUE(map->selectedKey() == Qt::Key_Escape);
        filter->setFocus();
        QTest::keyClick(filter, Qt::Key_Return);
        EXPECT_TRUE(mgr.action(kPtt)->currentKey == QKeySequence(Qt::Key_Escape));
        EXPECT_TRUE(dlg.isVisible());
    }

    // ---- the map is as tall as its keys, and past its cap a wider window
    //      leaves the table's height alone ----
    auto* table = dlg.findChild<QTableWidget*>();
    EXPECT_TRUE(map && table);
    if (map && table) {
        dlg.resize(1100, 840);
        QTest::qWait(50);
        EXPECT_TRUE(map->height() <= map->heightForWidth(map->width()) + 1);
        dlg.resize(1400, 840);
        QTest::qWait(50);
        const int tableAt1400 = table->height();
        dlg.resize(2000, 840);
        QTest::qWait(50);
        EXPECT_TRUE(table->height() == tableAt1400);
        EXPECT_TRUE(table->height() >= 250);
        dlg.resize(1100, 840);
        QTest::qWait(50);
    }

    // ---- the layout fits within the minimum, which fits a 768 px screen.
    //      An explicit minimum overrides the layout's own, so a body wider or
    //      taller than it would clip silently; the body sits inside the
    //      window's hairline inset on each side ----
    {
        EXPECT_TRUE(dlg.minimumHeight() <= 700);
        const QSize body = dlg.bodyWidget()->minimumSizeHint()
                         + QSize(2 * CanonWindow::kInset, 2 * CanonWindow::kInset);
        std::printf("  body minimum %dx%d with its inset, window minimum %dx%d\n",
                    body.width(), body.height(), dlg.minimumWidth(), dlg.minimumHeight());
        EXPECT_TRUE(body.height() <= dlg.minimumHeight());
        EXPECT_TRUE(body.width() <= dlg.minimumWidth());
    }

    // ---- a press on a key is the map's; one between keys is the window's ----
    if (map) {
        const QPointF corner(map->width() - 3, map->height() - 3);
        PressSpy spy;
        dlg.installEventFilter(&spy);
        QMouseEvent miss(QEvent::MouseButtonPress, corner, map->mapToGlobal(corner),
                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(map, &miss);
        QMouseEvent release(QEvent::MouseButtonRelease, corner, map->mapToGlobal(corner),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(map, &release);
        dlg.removeEventFilter(&spy);
        EXPECT_TRUE(spy.presses == 1);   // passed on to the window, which drags
        // The Esc key is the map's first key, in its top-left corner.
        const QPointF esc(map->width() * 0.03, map->height() * 0.12);
        QMouseEvent hit(QEvent::MouseButtonPress, esc, map->mapToGlobal(esc),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        hit.setAccepted(false);
        QCoreApplication::sendEvent(map, &hit);
        EXPECT_TRUE(hit.isAccepted());
        EXPECT_TRUE(map->selectedKey() == Qt::Key_Escape);
    }

    // ---- the Filter field's right-click menu keeps a canon panel, and an
    // unavailable item (Undo, on an untouched field) reads in the canon
    // unavailable ink rather than Qt's own disabled grey ----
    {
        QLineEdit* filter = nullptr;
        for (auto* edit : dlg.findChildren<QLineEdit*>()) {
            if (edit->placeholderText() == QStringLiteral("Search actions..."))
                filter = edit;
        }
        EXPECT_TRUE(filter != nullptr);
        if (filter) {
            QMenu* menu = filter->createStandardContextMenu();
            EXPECT_TRUE(menu->parentWidget() == filter);
            QAction* undo = menu->actions().value(0);
            EXPECT_TRUE(undo && !undo->isEnabled());
            menu->popup(dlg.mapToGlobal(QPoint(40, 40)));
            QTest::qWait(100);
            const QImage image = menu->grab().toImage();
            EXPECT_TRUE(!image.isNull()
                        && qAlpha(image.pixel(image.width() / 2, image.height() / 2)) == 255);
            // The glyph core is the row's pixel furthest from the panel. An
            // antialiased pixel is a blend of ink and panel, so however light
            // the hinting, it lies on the line from the panel to the ink.
            const QColor want = ThemeManager::instance().color(
                &dlg, QStringLiteral("color.control.unavailable"));
            const QColor panel = ThemeManager::instance().color(
                &dlg, QStringLiteral("color.canon.raised"));
            auto distance = [](const QColor& a, const QColor& b) {
                return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green())
                     + qAbs(a.blue() - b.blue());
            };
            QColor core = panel;
            const QRect row = undo ? menu->actionGeometry(undo) & image.rect() : QRect();
            for (int y = row.top(); y <= row.bottom(); ++y) {
                for (int x = row.left(); x <= row.right(); ++x) {
                    const QColor c = image.pixelColor(x, y);
                    if (distance(c, panel) > distance(core, panel))
                        core = c;
                }
            }
            // How far along that line the core sits (0 = panel, 1 = ink), and
            // how far off it, in 8-bit channel units.
            const double dr = want.red() - panel.red();
            const double dg = want.green() - panel.green();
            const double db = want.blue() - panel.blue();
            const double cr = core.red() - panel.red();
            const double cg = core.green() - panel.green();
            const double cb = core.blue() - panel.blue();
            const double len2 = dr * dr + dg * dg + db * db;
            const double along = len2 > 0 ? (cr * dr + cg * dg + cb * db) / len2 : 0;
            const double offR = cr - along * dr;
            const double offG = cg - along * dg;
            const double offB = cb - along * db;
            const double off = std::sqrt(offR * offR + offG * offG + offB * offB);
            std::printf("  Undo text core %s, want color.control.unavailable %s "
                        "(%.2f of the way from the panel, %.1f off the line)\n",
                        qPrintable(core.name()), qPrintable(want.name()), along, off);
            EXPECT_TRUE(along >= 0.5 && along <= 1.1);
            EXPECT_TRUE(off <= 8);
            menu->close();
            delete menu;
        }
    }

    // ---- a combo's drop-down list sits on canon.nested, as the other canon
    // windows' lists do; the right-click menu above is on canon.raised ----
    {
        QComboBox* combo = nullptr;
        for (auto* c : dlg.findChildren<QComboBox*>()) {
            if (c->isVisible() && c->count() >= 2)
                combo = c;
        }
        EXPECT_TRUE(combo != nullptr);
        if (combo) {
            combo->showPopup();
            QTest::qWait(150);
            QWidget* viewport = combo->view()->viewport();
            const QImage list = viewport->grab().toImage();
            const int rowH = combo->view()->sizeHintForRow(0);
            // The right end of the second row: unselected, past its text.
            const QColor ground = list.isNull()
                ? QColor()
                : list.pixelColor(QPoint(list.width() - 4, int(rowH * 1.5 * list.devicePixelRatio())));
            const QColor want = ThemeManager::instance().color(
                &dlg, QStringLiteral("color.canon.nested"));
            std::printf("  drop-down list ground %s, want color.canon.nested %s\n",
                        qPrintable(ground.name()), qPrintable(want.name()));
            EXPECT_TRUE(ground == want);
            combo->hidePopup();
        }
    }

    if (g_failures == 0) {
        std::printf("shortcut_dialog_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "shortcut_dialog_test: %d failure(s)\n", g_failures);
    return 1;
}
