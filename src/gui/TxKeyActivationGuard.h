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
// is the point: Space activates it. For a button that KEYS THE TRANSMITTER --
// anything carrying markTxKeying() / registerTxKeyingAction(), the same marker
// the automation bridge's TX guard reads (AutomationServer.cpp
// isTransmitControl) -- it is not: the app runs the Fusion style, where a
// clicked QPushButton takes focus, so the Space that used to be swallowed by
// the PTT-hold filter would now toggle MOX and latch transmit after the
// operator had merely clicked it with the mouse.
//
// So MainWindow::eventFilter() refuses exactly the keys that change was about:
// an activation key (Space, Select, Enter, Return) that is bound to a shortcut
// action, delivered to a TX-keying button, while shortcuts are off. That is the
// behaviour of origin/main before #5483 for those keys (the bound key never
// reached the button), and nothing wider:
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
    const auto* button = qobject_cast<const QAbstractButton*>(receiver);
    return button && button->property(kTxKeyingProperty).toBool()
        && txActionRequiresPermission(button);
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
    // Resolve the binding the way MainWindow resolves PTT (Hold):
    // shortcutSequenceFromKeyEvent() masks to these four modifiers.
    const Qt::KeyboardModifiers modifiers = ev->modifiers()
        & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    return shortcuts.actionForKey(QKeySequence(static_cast<int>(modifiers) | ev->key()))
        != nullptr;
}

} // namespace AetherSDR
