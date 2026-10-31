// MQTT Settings on the style guide's CanonWindow (RFC #6226): the fields'
// right-click menus keep a canon panel under the body sheet's transparent
// rule, and every way of closing the window destroys it. Socket-free: the dialog is never accepted, so no broker connection is
// made.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/CanonWindow.h"
#include "gui/MqttSettingsDialog.h"

#include <QApplication>
#include <QImage>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QTest>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* name)
{
    std::printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
    g_failures += !ok;
}

// A field's standard right-click menu is a child of the field, so the canon
// sheet's transparent-everything rule reaches it unless QMenu gets its panel.
void fieldMenuIsOpaque()
{
    MqttSettingsDialog dialog;
    check(qobject_cast<CanonWindow*>(&dialog) != nullptr, "MQTT Settings is a CanonWindow");
    dialog.show();
    check(QTest::qWaitForWindowExposed(&dialog), "the window is exposed");
    const QList<QLineEdit*> edits = dialog.bodyWidget()->findChildren<QLineEdit*>();
    check(!edits.isEmpty(), "the window has a text field");
    if (edits.isEmpty()) return;
    QMenu* menu = edits.first()->createStandardContextMenu();
    check(menu->parentWidget() == edits.first(),
          "control: the field's menu is parented inside the canon body");
    menu->popup(dialog.mapToGlobal(QPoint(40, 40)));
    QTest::qWait(100);
    const QImage image = menu->grab().toImage();
    check(!image.isNull() && qAlpha(image.pixel(image.width() / 2, image.height() / 2)) == 255,
          "a field's right-click menu has an opaque background");
    menu->close();
    delete menu;
}

// MainWindow opens the window with WA_DeleteOnClose. Every way out destroys
// it, Esc included (QDialog::done() closes, it does not only hide), so a
// half-edited field never comes back: the next open reloads the saved values.
void everyCloseDestroysTheWindow()
{
    const struct { const char* name; int path; } paths[] = {
        {"Esc destroys the window", 0},
        {"Ctrl+W / Cmd+W destroys the window", 1},
        {"the corner close button destroys the window", 2},
    };
    for (const auto& p : paths) {
        QPointer<MqttSettingsDialog> dialog = new MqttSettingsDialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
        if (!QTest::qWaitForWindowExposed(dialog)) {
            check(false, p.name);
            continue;
        }
        if (p.path == 0) QTest::keyClick(dialog, Qt::Key_Escape);
        if (p.path == 1) QTest::keySequence(dialog, QKeySequence::Close);
        if (p.path == 2) dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(dialog.isNull(), p.name);
        delete dialog.data();
    }
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("mqtt-settings-dialog");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();

    fieldMenuIsOpaque();
    everyCloseDestroysTheWindow();

    return g_failures ? 1 : 0;
}
