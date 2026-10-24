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
#include <QPushButton>
#include <QTest>
#include <QTimer>
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
// records whether one opened at all, and which button it opened on.
struct Answer {
    bool opened{false};
    QString defaultText;
    QString title;
};

void answerNextBox(Answer& seen, std::function<void(CanonMessageBox&)> act)
{
    QTimer::singleShot(0, [&seen, act] {
        auto* box = qobject_cast<CanonMessageBox*>(QApplication::activeModalWidget());
        if (!box) {
            return;
        }
        QTest::qWaitForWindowExposed(box);
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
    QTest::qWaitForWindowExposed(&dlg);

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
        emit combo->activated(muteIdx);
        EXPECT_TRUE(seen.opened);
        EXPECT_TRUE(seen.title == QStringLiteral("Key Conflict"));
        EXPECT_TRUE(seen.defaultText == QStringLiteral("Reassign"));
        EXPECT_TRUE(mgr.action(kPtt)->currentKey == QKeySequence(Qt::Key_Escape));
        EXPECT_TRUE(mgr.action(kMute)->currentKey.isEmpty());

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

    if (g_failures == 0) {
        std::printf("shortcut_dialog_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "shortcut_dialog_test: %d failure(s)\n", g_failures);
    return 1;
}
