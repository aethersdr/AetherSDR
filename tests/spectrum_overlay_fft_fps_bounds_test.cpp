// The Display panel's FFT FPS slider runs exactly the bounds
// ClientDisplaySettings accepts, so every slider position is storable and no
// stored value is off the slider. Widget only, offscreen, no backend.

#include "core/ClientDisplaySettings.h"
#include "gui/SpectrumOverlayMenu.h"

#include <QApplication>
#include <QSlider>
#include <QWidget>

#include <cstdio>

using namespace AetherSDR;

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QWidget parent;
    SpectrumOverlayMenu menu(&parent);

    const auto* slider = parent.findChild<QSlider*>(QStringLiteral("displayFftFpsSlider"));
    if (!slider) {
        std::fprintf(stderr, "FAIL: the FFT FPS slider is discoverable\n");
        return 1;
    }
    int failures = 0;
    if (slider->minimum() != ClientDisplaySettings::kFftFpsMin) {
        std::fprintf(stderr, "FAIL: slider minimum %d is kFftFpsMin\n", slider->minimum());
        ++failures;
    }
    if (slider->maximum() != ClientDisplaySettings::kFftFpsMax) {
        std::fprintf(stderr, "FAIL: slider maximum %d is kFftFpsMax\n", slider->maximum());
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
