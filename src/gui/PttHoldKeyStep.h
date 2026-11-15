#pragma once

#include <QEvent>

namespace AetherSDR {

// What MainWindow::handlePttHoldShortcut() does with a non-auto-repeat
// press/release of the PTT (Hold) key, once the key is known to be that
// binding. Kept free of MainWindow so window_shortcut_test can pin it.
enum class PttHoldKeyStep {
    PassThrough,  // not ours: let the key reach the focused widget
    Consume,      // ours, but nothing to key or unkey
    KeyTx,        // start the hold (key the transmitter)
    UnkeyTx,      // end the hold (un-key the transmitter)
};

// The release of a live hold always un-keys, and is checked first, before any
// gate: shortcuts switched off, focus moved into a text field or the connection
// dropped mid-hold must not leave the transmitter keyed. Only a new press is
// gated, as in handleSplitMonitorShortcut().
inline PttHoldKeyStep pttHoldKeyStep(QEvent::Type type, bool holdActive,
                                     bool shortcutsEnabled, bool textEntryCaptured,
                                     bool connected)
{
    if (type == QEvent::KeyRelease && holdActive)
        return PttHoldKeyStep::UnkeyTx;
    // Only key while connected and not typing into a text field; otherwise the
    // key is not ours and falls through.
    if (textEntryCaptured || !connected)
        return PttHoldKeyStep::PassThrough;
    // With keyboard shortcuts off the key did nothing, and eating it would
    // take Space away from the focused button as well (#5483).
    if (!shortcutsEnabled)
        return PttHoldKeyStep::PassThrough;
    if (type == QEvent::KeyPress && !holdActive)
        return PttHoldKeyStep::KeyTx;
    // Consume the bound key so it cannot also activate a button.
    return PttHoldKeyStep::Consume;
}

} // namespace AetherSDR
