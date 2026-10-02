#pragma once

#include "core/ShortcutManager.h"
#include "core/TxKeyingMarker.h"

#include <QAbstractButton>
#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>

namespace AetherSDR {

// Keyboard activation of a transmit-keying button while keyboard shortcuts are
// off (#5483). A bound key then reaches the focused widget, and a clicked
// button keeps focus, so Space would latch MOX. MainWindow::eventFilter()
// refuses an activation key (Space, Select, Enter, Return) that is bound to a
// shortcut action and delivered to a TX-keying button while shortcuts are off,
// whether or not a radio is connected.
//
// "Keys the transmitter" is transmitControlMatch() (core/TxKeyingMarker.h), the
// predicate the automation bridge uses, walked up the parent chain. Untouched:
// mouse clicks, an activation key bound to no shortcut, a screen reader's
// press action (no key event), and receive-only marked controls.
inline bool isTxKeyingButton(const QObject* receiver)
{
    // Only a button is activated by these keys; what it belongs to is judged
    // up its parent chain, as the bridge does.
    const auto* button = qobject_cast<const QAbstractButton*>(receiver);
    if (!button)
        return false;
    for (const QWidget* w = button; w; w = w->parentWidget()) {
        if (transmitControlMatch(w) != TransmitControlMatch::None
            && txActionRequiresPermission(w))
            return true;
    }
    return false;
}

inline bool refuseTxKeyActivation(const QObject* receiver, const QKeyEvent* ev,
                                  bool keyboardShortcutsEnabled,
                                  const ShortcutManager& shortcuts)
{
    if (keyboardShortcutsEnabled || !ev)
        return false;
    if (ev->type() != QEvent::KeyPress && ev->type() != QEvent::KeyRelease)
        return false;
    switch (ev->key()) {
    case Qt::Key_Space:
    case Qt::Key_Select:
    case Qt::Key_Enter:
    case Qt::Key_Return:
        break;
    default:
        return false;
    }
    if (!isTxKeyingButton(receiver))
        return false;
    // Resolve the binding exactly as MainWindow resolves PTT (Hold): the same
    // function, not a copy of it.
    return shortcuts.actionForKey(shortcutSequenceFromKeyEvent(ev)) != nullptr;
}

} // namespace AetherSDR
