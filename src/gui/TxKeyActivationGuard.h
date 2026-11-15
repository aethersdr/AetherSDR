#pragma once

#include "core/ShortcutManager.h"
#include "core/TxKeyingMarker.h"

#include <QAbstractButton>
#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>

namespace AetherSDR {

// With keyboard shortcuts off a bound key reaches the focused widget, and a
// clicked button keeps focus (#5483). Refuse a bound activation key (Space,
// Select, Enter, Return) on a button that transmitControlMatch() — the bridge's
// predicate, walked up the parent chain — says keys TX. Mouse clicks, unbound
// keys, a screen reader's press action and receive-only controls are untouched.
inline bool isTxKeyingButton(const QObject* receiver)
{
    // Only a button is activated by these keys; judge its whole parent chain.
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
    // The same resolver MainWindow uses for PTT (Hold).
    return shortcuts.actionForKey(shortcutSequenceFromKeyEvent(ev)) != nullptr;
}

} // namespace AetherSDR
