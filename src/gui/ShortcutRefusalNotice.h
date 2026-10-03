#pragma once

#include <QEvent>
#include <QKeyEvent>

namespace AetherSDR {

// The capture test a refused action would have met with shortcuts on. A hold
// key (PTT, CW keys, Monitor TX) runs from the event filter and yields to text
// entry only; a QShortcut action also yields to any combo and a slider lease.
inline bool shortcutRefusalInputCaptured(bool holdKeyAction, bool textEntryCaptured,
                                         bool shortcutInputCaptured)
{
    return holdKeyAction ? textEntryCaptured : shortcutInputCaptured;
}

// With keyboard shortcuts off a bound key does nothing, so the operator cannot
// tell "off" from "unbound" (#5483). take() returns the refused action for the
// first refused press in a session only, else nullptr: a key press, not
// auto-repeat, bound to an operating action, with the master switch off and
// nothing capturing the key. A notice that cannot be shown (the status bar is
// hidden in minimal mode) is not taken, so it is still there to give later.
// A template, so this header stays Qt-only.
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
