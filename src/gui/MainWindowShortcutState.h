#pragma once

// Shared state for MainWindow's shortcut system (#3351); only MainWindow*.cpp
// may include this. Definitions live in MainWindow.cpp. These are file-scope
// because ShortcutManager::rebuildShortcuts takes a receiver-less
// std::function<bool()> guard; MainWindow mirrors m_keyboardShortcutsEnabled
// into s_keyboardShortcutsEnabled.

class QWidget;

namespace AetherSDR {

// Global enable for keyboard shortcuts (Settings menu toggle).
extern bool s_keyboardShortcutsEnabled;

// True while a slider holds the keyboard-shortcut lease (#745) —
// arrow keys go to the slider, not to shortcut actions.
extern bool s_sliderShortcutLeaseActive;

// True when a text-input widget has focus (line edit, spin box, …).
// Includes non-editable combos so arrow shortcuts stay suppressed while a
// combo is focused (the arrows navigate its list).
bool textInputCaptured();

// True when a text-*entry* widget has focus (line edit, spin box, editable
// combo). Unlike textInputCaptured(), a focused non-editable combo does NOT
// count — used to gate TX keying (Space PTT / CW keys) so a combo that keeps
// focus after its popup closes can't swallow the keypress (#3908).
bool textEntryCaptured();

// textInputCaptured() plus the slider lease.
bool shortcutInputCaptured();

// The std::function<bool()> guard handed to ShortcutManager: shortcuts
// fire only when enabled and no input widget / lease captures keys.
bool shortcutGuard();

// True while the lease holder is actively being dragged with the mouse.
bool leaseHolderBusy(QWidget* w);

} // namespace AetherSDR
