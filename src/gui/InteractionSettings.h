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

} // namespace AetherSDR
