#pragma once

#include <functional>

class QSlider;

namespace AetherSDR {

// Wires a slider so that:
//   * `live(v)` fires on every `QSlider::valueChanged` tick (immediate
//     visual / DSP / radio feedback as the user drags — must not regress).
//   * `persist(v)` fires once per gesture, whichever of these happens first:
//       - `QSlider::sliderReleased` (mouse drag finished)
//       - 500 ms debounce after the last `valueChanged` (keyboard arrows,
//         mouse wheel, programmatic changes — none of which emit
//         `sliderReleased`)
//
// The debounce timer is parented to `slider`, so it is destroyed with the
// slider. If the application exits mid-debounce the most recent value is
// not persisted — acceptable for non-critical UI state; do not use this
// helper for state that must round-trip a crash.
//
// Centralizes the connect+save boilerplate that previously called
// `AppSettings::save()` on every tick — see issue #3032. The atomic XML
// rewrite cost meant a 2-second drag at 60 Hz triggered ~120× full
// sort+serialize+reparse+rename of the settings file.
void connectSliderSetting(QSlider* slider,
                          std::function<void(int)> live,
                          std::function<void(int)> persist);

} // namespace AetherSDR
