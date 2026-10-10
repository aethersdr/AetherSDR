#pragma once

#include <QLatin1String>

class QMenu;

namespace AetherSDR {

// The title bar's menu look (#6198), shared so every menu that adopts it
// stays identical: rounded, slightly see-through panels, 8 px like the radio
// tabs, with the selection inset clear of the curve. Item text starts at one
// x in every menu: a 14 px check column, and the same 14 px added to the
// padding of menus that have none.
inline constexpr QLatin1String kRoundedMenuRules{
    "QMenu { background: {{color.titlebar.menu.background}}; color: {{color.text.primary}};"
    " border: 1px solid {{color.background.2}}; border-radius: 8px; padding: 4px; }"
    "QMenu::item { padding: 4px 24px 4px 12px; }"
    "QMenu[aetherHasChecks=\"false\"]::item { padding-left: 26px; }"
    "QMenu::indicator { width: 14px; height: 14px; }"
    "QMenu::item:selected { background: {{color.background.2}}; border-radius: 4px; }"
    "QMenu::separator { height: 1px; background: {{color.background.2}}; margin: 4px 8px; }"};

// A stylesheet border-radius only shapes what is painted, so each menu also
// needs a see-through window and no system drop shadow (square on Windows),
// set before its native window is first created. Applies to `menu` and every
// submenu, including ones added later (re-checked on aboutToShow).
void roundMenuTree(QMenu* menu);

}  // namespace AetherSDR
