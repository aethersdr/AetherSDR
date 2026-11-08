// Callsign Lookup on the style guide's CanonWindow (RFC #6226), the way AetherRX
// and AetherTX are built: a frameless workspace window in the canon scope that
// restores the geometry the old PersistentDialog saved. Socket-free: no lookup
// is started, so the QRZ client never makes a request.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/CallsignCard.h"
#include "gui/CallsignLookupDialog.h"
#include "gui/CanonWindow.h"

#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QImage>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QStyle>
#include <QStyleOptionFrame>
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

void canonWorkspace()
{
    CallsignLookupDialog dialog;
    check(qobject_cast<CanonWindow*>(&dialog) != nullptr, "Callsign Lookup is a CanonWindow");
    check(dialog.windowType() == Qt::Window
              && dialog.windowFlags().testFlag(Qt::FramelessWindowHint),
          "it is a frameless workspace window, not a dialog on its parent");
    check(dialog.findChild<QLabel*>(QStringLiteral("canonHeaderTitle")) != nullptr,
          "it carries the canon header");
}

// The docs screenshot automation (docs/user/screens.json) and the bridge drive
// the window by these names.
void controlsKeepTheirNames()
{
    CallsignLookupDialog dialog;
    check(dialog.objectName() == QLatin1String("CallsignLookupDialog"), "the window keeps its name");
    check(dialog.findChild<QLineEdit*>(QStringLiteral("callsignLookupInput")) != nullptr,
          "the callsign field keeps its name");
    auto* go = dialog.findChild<QPushButton*>(QStringLiteral("callsignLookupGo"));
    check(go && go->isDefault(), "Lookup keeps its name and is the default button");
    check(dialog.findChild<QPushButton*>(QStringLiteral("callsignLookupRefresh")) != nullptr,
          "Refresh keeps its name");
    check(dialog.findChild<QLabel*>(QStringLiteral("callsignLookupStatus")) != nullptr,
          "the status line keeps its name");
}

// The contact card paints with base tokens; inside the window they resolve to
// the canon palette, so the card matches the window.
void cardFollowsTheCanonScope()
{
    CallsignLookupDialog dialog;
    auto* card = dialog.findChild<CallsignCard*>();
    check(card != nullptr, "the window holds a contact card");
    if (!card) return;
    auto& tm = ThemeManager::instance();
    QWidget outside;
    const QColor inside = tm.color(card, QStringLiteral("color.background.1"));
    const QColor root = tm.color(&outside, QStringLiteral("color.background.1"));
    check(inside.isValid() && inside != root,
          "the card's ground resolves through the canon scope, not the root theme");
}

// At the smallest the window can be, the callsign field still shows its whole
// placeholder: the field's minimum width comes from the placeholder in the
// canon sheet's monospace font, so the fit holds whichever face the platform
// resolves (SF Mono, Menlo, Consolas or a generic fallback).
void fieldFitsItsPlaceholderAtMinimumSize()
{
    CallsignLookupDialog dialog;
    dialog.resize(dialog.minimumSize());
    dialog.show();
    check(QTest::qWaitForWindowExposed(&dialog), "the window is exposed");
    auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("callsignLookupInput"));
    check(input != nullptr, "the callsign field exists");
    if (!input) return;
    QStyleOptionFrame opt;
    opt.initFrom(input);
    opt.lineWidth = input->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &opt, input);
    const QRect text = input->style()->subElementRect(QStyle::SE_LineEditContents, &opt, input)
                           .marginsRemoved(input->textMargins());
    const int needed = input->fontMetrics().horizontalAdvance(input->placeholderText());
    std::printf("  field %d px, text area %d px, placeholder %d px\n",
                input->width(), text.width(), needed);
    check(needed <= text.width(), "the callsign field shows its whole placeholder at minimum size");
    check(input->minimumWidth() >= needed + (input->width() - text.width()),
          "the field's minimum width is measured from its placeholder, not a fixed window size");
}

// The field's standard right-click menu is a child of the field, so the canon
// sheet's transparent-everything rule reaches it unless QMenu gets its panel.
void rightClickMenuIsOpaque()
{
    CallsignLookupDialog dialog;
    dialog.show();
    check(QTest::qWaitForWindowExposed(&dialog), "the window is exposed");
    auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("callsignLookupInput"));
    if (!input) return;
    QMenu* menu = input->createStandardContextMenu();
    check(menu->parentWidget() == input, "control: the field's menu is parented inside the canon body");
    menu->popup(dialog.mapToGlobal(QPoint(40, 40)));
    QTest::qWait(100);
    const QImage image = menu->grab().toImage();
    check(!image.isNull() && qAlpha(image.pixel(image.width() / 2, image.height() / 2)) == 255,
          "the field's right-click menu has an opaque background");
    menu->close();
    delete menu;
}

// Size and position persist under CallsignLookupDialogGeometry, the key the old
// dialog wrote, including its base64 rectangles.
void geometryCarriesOver()
{
    static const QString kKey = QStringLiteral("CallsignLookupDialogGeometry");
    auto& settings = AppSettings::instance();
    settings.remove(kKey);

    CallsignLookupDialog restored;
    const QSize chosen(restored.minimumWidth() + 60, restored.minimumHeight() + 110);
    QDialog legacy;
    legacy.resize(chosen);
    settings.setValue(kKey, QString::fromLatin1(legacy.saveGeometry().toBase64()));
    check(restored.size() != chosen, "control: a fresh window is not born at the saved size");
    restored.show();
    check(QTest::qWaitForWindowExposed(&restored), "the window is exposed");
    for (int i = 0; i < 50 && restored.size() != chosen; ++i) {
        QTest::qWait(20);
    }
    check(restored.size() == chosen, "geometry saved by the old dialog restores under its key");

    const QSize resized = chosen + QSize(40, 20);
    restored.resize(resized);
    QCoreApplication::processEvents();
    restored.hide();
    const QString saved = settings.value(kKey).toString();
    check(saved.endsWith(QStringLiteral(",%1,%2").arg(resized.width()).arg(resized.height())),
          "hiding the window saves its size under the same key");
}

// MainWindow opens the window with WA_DeleteOnClose. Every way out destroys
// it, Esc included (QDialog::done() closes, it does not only hide), so the
// next open is a fresh window with an empty field.
void everyCloseDestroysTheWindow()
{
    for (int path = 0; path < 3; ++path) {
        QPointer<CallsignLookupDialog> dialog = new CallsignLookupDialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
        const bool exposed = QTest::qWaitForWindowExposed(dialog);
        if (path == 0) QTest::keyClick(dialog, Qt::Key_Escape);
        if (path == 1) QTest::keySequence(dialog, QKeySequence::Close);
        if (path == 2) dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(exposed && dialog.isNull(),
              path == 0 ? "Esc destroys the window"
              : path == 1 ? "Ctrl+W / Cmd+W destroys the window"
                          : "the corner close button destroys the window");
        delete dialog.data();
    }
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("callsign-lookup-dialog");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();

    canonWorkspace();
    controlsKeepTheirNames();
    cardFollowsTheCanonScope();
    fieldFitsItsPlaceholderAtMinimumSize();
    rightClickMenuIsOpaque();
    everyCloseDestroysTheWindow();
    geometryCarriesOver();

    return g_failures ? 1 : 0;
}
