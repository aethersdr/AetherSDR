#pragma once

#include "core/AppSettings.h"

#include <QApplication>

namespace AetherSDR {

// Centralized accessor for the "click-discrimination interval" — the time
// AetherSDR waits after a single click on a widget that has double-click
// semantics before firing the single-click action.  Several widgets defer
// their single-click handler by this interval so a double click within the
// window can override it (e.g. RxApplet's slice-mute button: single click
// mutes this slice, double click mutes all owned slices).
//
// Default is the platform's QApplication::doubleClickInterval() (typically
// 400 ms on Linux, 500 ms on Windows, system-configurable on macOS).  Users
// can override it via Radio Setup → Behavior to trade off single-click
// latency vs. double-click registration reliability.  A value of 0 disables
// the defer entirely — single-click actions fire instantly, and double-click
// affordances become unreachable on widgets that use this knob.
//
// All call sites should read the value at click time rather than caching it
// at construction so changes via Settings propagate without an app restart.
inline int clickDiscriminationIntervalMs()
{
    auto& s = AppSettings::instance();
    bool ok = false;
    const int v = s.value("ClickDiscriminationIntervalMs",
                          QApplication::doubleClickInterval()).toInt(&ok);
    if (!ok || v < 0)
        return QApplication::doubleClickInterval();
    return v;
}

// Whether mouse-wheel tuning should be reversed (clockwise = down).  Matches
// the comparable option in Thetis / KE9NS — useful for trackballs where the
// natural wheel direction feels inverted (#3302).  Read per-event at the
// wheel handler so toggling via Radio Setup → UI Enhancements takes effect
// immediately without an app restart.  Only the frequency-tuning paths in
// VfoWidget::wheelEvent and SpectrumWidget::wheelEvent consult this; the
// Ctrl+wheel bandwidth zoom is intentionally not reversed.
inline bool reverseMouseWheel()
{
    auto& s = AppSettings::instance();
    return s.value("ReverseMouseWheel", false).toBool();
}

} // namespace AetherSDR
