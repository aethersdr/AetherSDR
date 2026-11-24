#pragma once

namespace AetherSDR::AppletRailMetrics {

// Widths of the docked applet rail, shared by AppletPanel and the tests that
// check an applet fits it. A test laying out at kRailWidth sees the scrollbar
// track that a docked applet never gets.
//
// The track is only present while the bar shows (the policy is
// ScrollBarAsNeeded), so a stacked applet gets kStackedApplet with the bar and
// kRailWidth without it.
inline constexpr int kRailWidth      = 260;
inline constexpr int kRailScrollBarW = 12;
inline constexpr int kStackedApplet  = kRailWidth - kRailScrollBarW;

static_assert(kStackedApplet == 248, "rail metrics drifted; update the tests too");

}  // namespace AetherSDR::AppletRailMetrics
