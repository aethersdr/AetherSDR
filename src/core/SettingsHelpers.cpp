#include "SettingsHelpers.h"

#include <QObject>
#include <QSlider>
#include <QTimer>

#include <utility>

namespace AetherSDR {

namespace {
// 500 ms after the last valueChanged tick before the persist callback
// fires. Long enough for users dragging via keyboard arrows or scroll
// wheel to settle on a value; short enough that a crash window after
// release is bounded.
constexpr int kPersistDebounceMs = 500;
} // namespace

void connectSliderSetting(QSlider* slider,
                          std::function<void(int)> live,
                          std::function<void(int)> persist)
{
    auto* debounce = new QTimer(slider);
    debounce->setSingleShot(true);
    debounce->setInterval(kPersistDebounceMs);

    // Reads the slider's current value at fire time so the persisted value
    // matches what the user sees, even if valueChanged ran but the timer
    // restart somehow didn't re-arm with the final tick.
    QObject::connect(debounce, &QTimer::timeout, slider, [slider, persist]() {
        persist(slider->value());
    });

    QObject::connect(slider, &QSlider::valueChanged, slider,
                     [debounce, live = std::move(live)](int v) {
        live(v);
        debounce->start();  // restart resets the 500 ms window
    });

    QObject::connect(slider, &QSlider::sliderReleased, slider,
                     [slider, debounce, persist = std::move(persist)]() {
        debounce->stop();
        persist(slider->value());
    });
}

} // namespace AetherSDR
