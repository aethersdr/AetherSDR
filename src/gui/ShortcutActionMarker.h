#pragma once

#include <QString>
#include <QWidget>

namespace AetherSDR {

// Names the shortcut id of the action a control performs, so ShortcutTooltips
// appends its bound key (RFC #5484). Set it where the control is created, like
// markTxKeying:
//     markShortcutAction(m_moxBtn, "mox_toggle");
// Do not mark a control that ControlAvailabilityRegistry writes: two writers
// would compose the same tooltip. ShortcutTooltips finds marked controls on
// refresh(); one built later is watched once passed to annotate().
inline constexpr char kShortcutActionProperty[] = "aetherShortcutAction";

inline void markShortcutAction(QWidget* w, const char* shortcutId)
{
    if (w) {
        w->setProperty(kShortcutActionProperty, QString::fromLatin1(shortcutId));
    }
}

} // namespace AetherSDR
