#pragma once

#include "core/ShortcutManager.h"
#include "core/TxKeyingMarker.h"

#include <QAbstractButton>
#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>

namespace AetherSDR {

// Keyboard activation of a transmit-keying button while keyboard shortcuts are
// OFF (#5483 follow-up).
//
// With shortcuts off, the operating QShortcuts are disabled so a bound key
// reaches the focused widget instead of vanishing. For an ordinary button that
// is the point: Space activates it. For a button that KEYS THE TRANSMITTER it
// is not: the app runs the Fusion style, where a clicked QPushButton takes
// focus, so the Space that used to be swallowed by the PTT-hold filter would
// now toggle MOX and latch transmit after the operator had merely clicked it
// with the mouse.
//
// "Keys the transmitter" is decided by the SAME predicate the automation
// bridge's TX guard uses -- transmitControlMatch() in core/TxKeyingMarker.h:
// the markTxKeying() / registerTxKeyingAction() marker, or the button-scoped
// name fallback for a keying control that forgot the marker -- walked up the
// parent chain as the bridge's hasTransmitControlInChain() does. One function,
// so the two guards cannot drift apart.
//
// So MainWindow::eventFilter() refuses exactly the keys that change was about:
// an activation key (Space, Select, Enter, Return) that is bound to a shortcut
// action, delivered to a TX-keying button, while shortcuts are off. While the
// radio is connected and the operator is not typing, that is the behaviour of
// origin/main before #5483 for those keys (the PTT (Hold) filter swallowed the
// bound key, so it never reached the button). It is slightly wider in one
// case, in the safe direction: on main, with the radio DISCONNECTED, the filter
// let the key through and Space clicked a focused MOX; this guard refuses that
// too. Nothing else is widened:
//   - Mouse clicks are not key events and are untouched.
//   - An activation key that is NOT bound to any shortcut is untouched, so a
//     keyboard path that exists today -- e.g. Return on an auto-default
//     TX-keying button in a dialog, or Space on MOX after PTT (Hold) was
//     rebound to another key -- keeps working exactly as before.
//   - A screen reader's "press" (VoiceOver VO+Space, NVDA/JAWS default action)
//     goes through QAccessibleActionInterface::pressAction(), which clicks the
//     button without a key event, so it is untouched too.
//   - Receive-only controls that carry the marker only for the bridge
//     (registerReceiveControlAction) are not transmit keys and are untouched.
// What a keyboard-only operator does to key with shortcuts off: the screen
// reader press action above, or switch keyboard shortcuts on and use PTT (Hold)
// (Space by default) or MOX Toggle (T by default). With shortcuts on this guard
// never fires; the PTT-hold filter consumes its key first, as before.
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
