#pragma once

// Internal shared state for MainWindow's keyboard-shortcut system
// (#3351 Phase 1b). NOT a public API — only MainWindow*.cpp TUs may
// include this.
//
// These were file-scope statics in MainWindow.cpp. The monolith split
// puts their writers in more than one translation unit (the View-menu
// toggle lives in MainWindow_Menus.cpp; the slider-lease lifecycle and
// the guard's readers stay in MainWindow.cpp until the Shortcuts TU
// lands), so they need external linkage. Definitions remain in
// MainWindow.cpp.
//
// Why file-scope state exists at all: ShortcutManager::rebuildShortcuts
// takes a plain std::function<bool()> guard with no receiver object, so
// the guard cannot read MainWindow members; it reads these flags
// instead. MainWindow mirrors m_keyboardShortcutsEnabled into
// s_keyboardShortcutsEnabled whenever it changes.

class QWidget;

namespace AetherSDR {

// Global enable for keyboard shortcuts (View menu toggle).
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
