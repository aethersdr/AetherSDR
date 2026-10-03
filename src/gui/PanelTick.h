#pragma once

namespace AetherSDR {

// Poll/repaint cadence for every animated panel widget: 60 Hz. One constant so
// meters sharing a window step together. A repaint rate only:
// MeterSmoother::tick() integrates wall-clock ms, so ballistics do not depend
// on it. Every timer on this cadence must stop while its widget is hidden (a
// QStackedWidget page hides when another tab is selected; Qt skips the paint
// but not the poll).
constexpr int kPanelTickMs = 16;

// The same cadence as a frame rate, for widgets whose API takes fps
// rather than an interval.
constexpr int kPanelTickHz = 1000 / kPanelTickMs;

} // namespace AetherSDR
