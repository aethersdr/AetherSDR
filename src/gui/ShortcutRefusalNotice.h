#pragma once

#include <QEvent>
#include <QKeyEvent>
#include <QWidget>

namespace AetherSDR {

// The capture test a refused action would have met with shortcuts on. A hold
// key (PTT, CW keys, Monitor TX) runs from the event filter and yields to text
// entry only; a QShortcut action also yields to any combo and a slider lease.
inline bool shortcutRefusalInputCaptured(bool holdKeyAction, bool textEntryCaptured,
                                         bool shortcutInputCaptured)
{
    return holdKeyAction ? textEntryCaptured : shortcutInputCaptured;
}

// A key no widget accepted reaches the window's keyPressEvent(). The TX key
// guard consumes its key in the event filter, so that press is noticed at the
// guard instead, and only for a receiver in the same window.
inline bool shortcutRefusalReceiverInWindow(const QObject* receiver, const QWidget* window)
{
    const auto* widget = qobject_cast<const QWidget*>(receiver);
    return widget && window && widget->window() == window;
}

// With keyboard shortcuts off a bound key does nothing, so the operator cannot
// tell "off" from "unbound" (#5483). take() returns the refused action for the
// first refused press in a session, else nullptr. A notice that cannot be
// shown (minimal mode hides the status bar) is not taken. A template, so
// this header includes no engine header.
class ShortcutRefusalNotice {
public:
    template <typename Action>
    const Action* take(const QKeyEvent* ev, bool shortcutsEnabled,
                       bool inputCaptured, bool noticeVisible,
                       const Action* operatingAction)
    {
        if (m_given || shortcutsEnabled || inputCaptured || !noticeVisible
                || !operatingAction)
            return nullptr;
        if (!ev || ev->type() != QEvent::KeyPress || ev->isAutoRepeat())
            return nullptr;
        m_given = true;
        return operatingAction;
    }

    bool given() const { return m_given; }

private:
    bool m_given{false};
};

} // namespace AetherSDR
