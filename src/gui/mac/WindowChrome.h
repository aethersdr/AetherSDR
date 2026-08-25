#pragma once

#include <QtGlobal>

class QWidget;

#ifndef Q_OS_MAC

// Off-macOS the whole namespace collapses to inline no-ops so shared chrome
// code can call it unconditionally instead of sprinkling #ifdefs.
namespace AetherSDR::mac {
inline bool isSupported() { return false; }
inline void applyFramelessWindowStyle(QWidget*, int) {}
} // namespace AetherSDR::mac

#else

namespace AetherSDR::mac {

// macOS trim for the frameless main window.
//
// AetherSDR draws its own 52 px title bar on every platform, macOS included.
// The alternative — keeping the real NSWindow title bar and painting Qt widgets
// into the same strip — was tried and abandoned: AppKit's title-bar container
// sits above Qt's content view and owns both the top rows of pixels and the
// mouse events in them, so the bar's own controls went dead and the brand and
// radio tabs rendered a row low.  Reconciling the two by hand meant resizing
// AppKit's private container, carving the strip out of the window's backdrop
// paint, and re-asserting the style mask behind Qt's back — three workarounds
// for a conflict that simply does not exist once the window is frameless.
//
// What is left here is trim, not layout: a borderless NSWindow has square
// corners and, once Qt clears the resizable bit, no edge resizing.  Both come
// straight back from the style mask and the content view's layer, so the window
// still looks and resizes like a Mac window.
//
// No-ops on a widget with no native NSWindow yet, so callers may invoke this
// before show() without guarding.

// True when running on macOS.  Always false elsewhere.
bool isSupported();

// Restore the rounded corners, drop shadow and native edge-resizing that
// Qt::FramelessWindowHint takes away, and install the 52 px visual-effect
// material behind Qt's translucent title-bar tint. Idempotent; safe to call
// again after Qt re-creates the window (which it does whenever flags change).
void applyFramelessWindowStyle(QWidget* window, int cornerRadius);

} // namespace AetherSDR::mac

#endif // Q_OS_MAC
