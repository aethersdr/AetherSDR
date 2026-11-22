#pragma once

#include "core/TxKeyingMarker.h"

#include <QWidget>

namespace AetherSDR {

// MainWindow::eventFilter's slider keyboard lease sets the leased slider's value
// from arrows, PageUp/PageDown and Home/End, bound or not. A bridge key press
// must not do that to a drive setpoint while the power ceiling is armed: End
// goes to maximum() past any ceiling. Every such key is refused, a step down
// too; TX permission gates keying only (#5851).
inline bool bridgeKeyMovesTxSetpoint(const QWidget* leasedSlider, int key, bool txCeilingArmed)
{
    if (!txCeilingArmed || !leasedSlider) {
        return false;
    }
    switch (key) {
    case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down:
    case Qt::Key_PageUp: case Qt::Key_PageDown: case Qt::Key_Home: case Qt::Key_End:
        break;
    default:
        return false;
    }
    for (const QWidget* w = leasedSlider; w; w = w->parentWidget()) {
        if (isTxSetpointControl(w)) {
            return true;
        }
    }
    return false;
}

} // namespace AetherSDR
