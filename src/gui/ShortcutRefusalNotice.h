#pragma once

#include <QEvent>
#include <QKeyEvent>

namespace AetherSDR {

// With keyboard shortcuts off a bound key does nothing, so the operator cannot
// tell "off" from "unbound" (#5483). take() is true for the first refused press
// in a session only: a key press, not auto-repeat, bound to an operating
// action, with the master switch off and no text field or slider holding keys.
class ShortcutRefusalNotice {
public:
    bool take(const QKeyEvent* ev, bool shortcutsEnabled, bool inputCaptured,
              bool boundToOperatingAction)
    {
        if (m_given || shortcutsEnabled || inputCaptured || !boundToOperatingAction)
            return false;
        if (!ev || ev->type() != QEvent::KeyPress || ev->isAutoRepeat())
            return false;
        m_given = true;
        return true;
    }

    bool given() const { return m_given; }

private:
    bool m_given{false};
};

} // namespace AetherSDR
