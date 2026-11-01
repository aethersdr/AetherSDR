// CanonMessageBox (RFC #6226): the canon window's question box. The caller
// acts on clickedButton(), so it must name the button the operator clicked
// (or Return clicked for them), and be null when the box is closed instead.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/CanonMessageBox.h"

#include <QApplication>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QPushButton>
#include <QTest>
#include <QTimer>
#include <QToolButton>
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

struct Box {
    CanonMessageBox box{QStringLiteral("Install AetherCTR2?"),
                        QStringLiteral("Install AetherCTR2 0.9.6 over the USB cable?")};
    QPushButton* cancel{box.addButton(QStringLiteral("Cancel"), CanonMessageBox::Role::Reject)};
    QPushButton* install{box.addButton(QStringLiteral("Install"), CanonMessageBox::Role::Accept)};

    Box() { box.setDefaultButton(install); }

    // Runs the box modally, doing `act` once it is showing.
    int run(std::function<void(CanonMessageBox&)> act)
    {
        QTimer::singleShot(0, &box, [this, act] {
            QTest::qWaitForWindowExposed(&box);
            act(box);
        });
        return box.exec();
    }
};

void pressKey(CanonMessageBox& box, Qt::Key key)
{
    QWidget* target = QApplication::focusWidget() ? QApplication::focusWidget() : &box;
    QTest::keyClick(target, key);
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-canon-message-box-test"));
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

    // ---- a click on an accepting button names it ----
    {
        Box b;
        const int result = b.run([&b](CanonMessageBox&) { b.install->click(); });
        EXPECT_TRUE(result == QDialog::Accepted);
        EXPECT_TRUE(b.box.clickedButton() == b.install);
    }

    // ---- a click on a rejecting button names it, and rejects ----
    {
        Box b;
        const int result = b.run([&b](CanonMessageBox&) { b.cancel->click(); });
        EXPECT_TRUE(result == QDialog::Rejected);
        EXPECT_TRUE(b.box.clickedButton() == b.cancel);
    }

    // ---- Escape closes it with no button clicked ----
    {
        Box b;
        const int result = b.run([](CanonMessageBox& box) { pressKey(box, Qt::Key_Escape); });
        EXPECT_TRUE(result == QDialog::Rejected);
        EXPECT_TRUE(b.box.clickedButton() == nullptr);
    }

    // ---- the corner close button closes it with no button clicked ----
    {
        Box b;
        const int result = b.run([](CanonMessageBox& box) {
            // The corner button is the window's only QToolButton.
            if (auto* close = box.findChild<QToolButton*>())
                close->click();
            else
                box.done(-1);
        });
        EXPECT_TRUE(result == QDialog::Rejected);
        EXPECT_TRUE(b.box.clickedButton() == nullptr);
    }

    // ---- Return clicks the default button, which opens with the focus ----
    {
        Box b;
        bool focused = false;
        const int result = b.run([&b, &focused](CanonMessageBox& box) {
            focused = b.install->hasFocus();
            pressKey(box, Qt::Key_Return);
        });
        EXPECT_TRUE(focused);
        EXPECT_TRUE(result == QDialog::Accepted);
        EXPECT_TRUE(b.box.clickedButton() == b.install);
        EXPECT_TRUE(b.install->isDefault() && !b.cancel->isDefault());
    }

    // ---- a box shown again forgets the previous answer ----
    {
        Box b;
        b.run([&b](CanonMessageBox&) { b.install->click(); });
        const int result = b.run([](CanonMessageBox& box) { pressKey(box, Qt::Key_Escape); });
        EXPECT_TRUE(result == QDialog::Rejected);
        EXPECT_TRUE(b.box.clickedButton() == nullptr);
    }

    // ---- markup in a manifest string is shown as text, never rendered ----
    {
        CanonMessageBox box(QStringLiteral("Install <b>X</b>?"),
                            QStringLiteral("Install <i>X</i> 1.0?"));
        box.setInformativeText(QStringLiteral("<a href=\"x\">y</a>"));
        int plain = 0;
        for (QLabel* l : box.findChildren<QLabel*>()) {
            if (l->text().contains(QLatin1Char('<')))
                plain += l->textFormat() == Qt::PlainText ? 1 : -100;
        }
        EXPECT_TRUE(plain == 3);   // title, text and further text
    }

    // ---- Return on a focused button clicks that button, not the default ----
    // Buttons are auto-default for this. A platform theme whose button-press
    // keys include Return (as here, offscreen) gives the same result without
    // it; one that leaves Return out would click the default instead.
    {
        Box b;
        bool cancelFocused = false;
        const int result = b.run([&b, &cancelFocused](CanonMessageBox& box) {
            b.cancel->setFocus();
            cancelFocused = QApplication::focusWidget() == b.cancel;
            pressKey(box, Qt::Key_Return);
        });
        EXPECT_TRUE(cancelFocused);
        EXPECT_TRUE(result == QDialog::Rejected);
        EXPECT_TRUE(b.box.clickedButton() == b.cancel);
    }

    // ---- the further text shows only when there is some; a destructive
    //      button accepts and is marked for the warning color ----
    {
        CanonMessageBox box(QStringLiteral("Restore the backup?"), QStringLiteral("Restore it?"));
        auto* info = box.findChild<QLabel*>(QStringLiteral("canonMessageInfo"));
        EXPECT_TRUE(info && info->isHidden());
        EXPECT_TRUE(box.accessibleDescription() == QStringLiteral("Restore it?"));
        box.setInformativeText(QStringLiteral("Only onto the CTR2-Max it came from."));
        EXPECT_TRUE(info && !info->isHidden());
        // A screen reader reads both texts when the box opens.
        EXPECT_TRUE(box.accessibleDescription()
                    == QStringLiteral("Restore it?\nOnly onto the CTR2-Max it came from."));
        QPushButton* restore =
            box.addButton(QStringLiteral("Restore"), CanonMessageBox::Role::Destructive);
        EXPECT_TRUE(restore->property("role").toString() == QStringLiteral("destructive"));
        QTimer::singleShot(0, &box, [restore] { restore->click(); });
        EXPECT_TRUE(box.exec() == QDialog::Accepted);
        EXPECT_TRUE(box.clickedButton() == restore);
    }

    // ---- the details show only when there are some, as text to scroll and
    //      copy, without taking the focus from the default button ----
    {
        Box b;
        auto* details = b.box.findChild<QPlainTextEdit*>(QStringLiteral("canonMessageDetails"));
        EXPECT_TRUE(details && details->isHidden());
        QStringList rows;
        for (int i = 0; i < 200; ++i)
            rows << QStringLiteral("Line %1: invalid ACTION_ID '<b>x</b>'.").arg(i + 2);
        b.box.setDetailedText(rows.join(QLatin1Char('\n')));
        bool shown = false;
        bool scrolls = false;
        bool focused = false;
        b.run([&](CanonMessageBox& box) {
            shown = details && details->isVisible();
            // The 200 lines scroll inside the panel; none is cut off.
            scrolls = details && details->verticalScrollBar()->maximum() > 0
                      && details->height() <= CanonMessageBox::kDetailsHeight;
            focused = b.install->hasFocus();
            pressKey(box, Qt::Key_Escape);
        });
        EXPECT_TRUE(shown && scrolls && focused);
        EXPECT_TRUE(details && details->isReadOnly());
        EXPECT_TRUE(details && details->toPlainText().startsWith(
                                   QStringLiteral("Line 2: invalid ACTION_ID '<b>x</b>'.")));
        b.box.setDetailedText({});
        EXPECT_TRUE(details && details->isHidden());
    }

    if (g_failures == 0) {
        std::printf("canon_message_box_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "canon_message_box_test: %d failure(s)\n", g_failures);
    return 1;
}
