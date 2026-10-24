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
#include <QLineEdit>
#include <QPushButton>
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
    geometryCarriesOver();

    return g_failures ? 1 : 0;
}
