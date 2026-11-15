#pragma once

#include <QPushButton>
#include <QElapsedTimer>

namespace AetherSDR {

// LIMIT toggle with a built-in activity indicator.  When the user
// enables the limiter (checkable), the button reads as "armed".
// Whenever ClientComp::limiterActive() returns true, the caller
// strobes the button via setActive(true) — the button then glows
// red for ~120 ms before decaying back to "armed" so brief firings
// still register visually.
//
// Kept as a subclass so the paint logic stays self-contained and
// the editor can treat it as a drop-in for QPushButton.
class ClientCompLimiterButton : public QPushButton {
    Q_OBJECT

public:
    explicit ClientCompLimiterButton(QWidget* parent = nullptr);

    // Called from the editor's meter-poll tick with the latest
    // limiterActive state.  Transitions from false→true restart the
    // hold timer; the button redraws accordingly.
    void setActive(bool active);

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    bool m_active{false};
    QElapsedTimer m_activeHold;
    // 500 ms hold after the last active tick — long enough that a brief
    // limiter firing still leaves a visible red flash, short enough that
    // sustained program material doesn't leave the button constantly
    // red after the signal goes quiet.
    static constexpr int kHoldMs = 500;
};

} // namespace AetherSDR
