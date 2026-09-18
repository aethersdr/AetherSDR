#pragma once

namespace AetherSDR {

// The cadence every animated panel widget polls its engine object and
// repaints at: 60 Hz, one tick per display frame on the common case.
//
// It is one constant because these panels share a window. AetherRX stacks
// seven of them and the Aetherial strip shows several at once, and when each
// picked its own rate — 25, 30 and 120 Hz all appeared — meters sitting side
// by side stepped at visibly different times.
//
// This is a repaint rate, not a ballistics parameter: MeterSmoother::tick()
// integrates against wall-clock milliseconds, so a meter driven at this rate
// has exactly the attack and release it had at any other.
//
// Every timer on this cadence must stop while its widget is hidden. A page in
// a QStackedWidget is hidden whenever another tab is selected, and Qt drops
// the repaint but not the poll behind it, so a panel that keeps ticking is
// reading the engine and stepping meters for something nobody can see.
constexpr int kPanelTickMs = 16;

// The same cadence as a frame rate, for widgets whose API takes fps
// rather than an interval.
constexpr int kPanelTickHz = 1000 / kPanelTickMs;

} // namespace AetherSDR
