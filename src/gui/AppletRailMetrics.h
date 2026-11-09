#pragma once

namespace AetherSDR::AppletRailMetrics {

// Widths of the docked applet rail, shared by AppletPanel and the tests that
// check an applet fits it. A test laying out at kRailWidth sees 18 px that a
// docked applet never gets.
//
// The outer gutter faces the window wall the rail is docked to. The scrollbar
// track is only present while the bar shows (the policy is ScrollBarAsNeeded),
// so a stacked applet gets kStackedApplet with the bar and kRailWidth minus
// the gutter without it.
inline constexpr int kRailWidth       = 260;
inline constexpr int kRailOuterGutter = 6;
inline constexpr int kRailScrollBarW  = 12;
inline constexpr int kStackedApplet   = kRailWidth - kRailOuterGutter - kRailScrollBarW;

static_assert(kStackedApplet == 242, "rail metrics drifted; update the tests too");

}  // namespace AetherSDR::AppletRailMetrics
