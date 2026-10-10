#include "RoundedMenu.h"

#include <QAction>
#include <QMenu>
#include <QStyle>
#include <QVariant>

namespace AetherSDR {

namespace {

constexpr const char* kRoundedMenuProperty = "aetherRoundedMenu";

// Qt reserves a check-mark column only in a menu that has a checkable item,
// so text would start further left in every other menu.  The stylesheet pads
// those by the same width (`aetherHasChecks="false"`); the flag is refreshed
// before each show because MainWindow edits some menus at runtime.
constexpr const char* kHasChecksProperty = "aetherHasChecks";

void syncMenuCheckColumn(QMenu* menu)
{
    bool hasChecks = false;
    for (const QAction* action : menu->actions()) {
        hasChecks = hasChecks || action->isCheckable();
    }
    const QVariant current = menu->property(kHasChecksProperty);
    if (current.isValid() && current.toBool() == hasChecks) {
        return;
    }
    menu->setProperty(kHasChecksProperty, hasChecks);
    menu->style()->unpolish(menu);
    menu->style()->polish(menu);
}

}  // namespace

void roundMenuTree(QMenu* menu)
{
    if (!menu || menu->property(kRoundedMenuProperty).toBool()) {
        return;
    }
    menu->setProperty(kRoundedMenuProperty, true);
    menu->setAttribute(Qt::WA_TranslucentBackground);
    menu->setWindowFlag(Qt::NoDropShadowWindowHint);
    const auto prepare = [menu]() {
        syncMenuCheckColumn(menu);
        for (QAction* action : menu->actions()) {
            roundMenuTree(action->menu());
        }
    };
    prepare();
    QObject::connect(menu, &QMenu::aboutToShow, menu, prepare);
}

}  // namespace AetherSDR
