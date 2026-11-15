#pragma once

#include <functional>

class QSlider;

namespace AetherSDR {

// `live(v)` fires on every valueChanged tick; `persist(v)` fires once per
// gesture, on sliderReleased or 500 ms after the last change (keyboard, wheel
// and programmatic changes don't emit sliderReleased) (#3032). The debounce
// timer is parented to `slider`; a value pending at exit is lost, so don't use
// this for state that must survive a crash.
void connectSliderSetting(QSlider* slider,
                          std::function<void(int)> live,
                          std::function<void(int)> persist);

} // namespace AetherSDR
