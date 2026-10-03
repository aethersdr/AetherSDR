#pragma once

#include "core/AppSettings.h"

#include <QApplication>

namespace AetherSDR {

// Click-discrimination interval: how long a widget with double-click semantics
// defers its single-click action (e.g. slice mute vs mute-all). Defaults to
// QApplication::doubleClickInterval(); overridable in Radio Setup → Behavior;
// 0 fires single-click instantly and makes double-click unreachable. Read at
// click time, never cached.
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
