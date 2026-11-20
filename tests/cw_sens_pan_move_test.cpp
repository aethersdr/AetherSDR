// #7314: exercise the outgoing flush and incoming restore on two applets.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/PanadapterApplet.h"
#include "models/CwDecodeSettings.h"

#include <QApplication>
#include <QEventLoop>
#include <QSlider>
#include <QTimer>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace {

QSlider* sensitivitySlider(PanadapterApplet& pan)
{
    for (QSlider* slider : pan.findChildren<QSlider*>()) {
        if (slider->accessibleName() == QStringLiteral("CW decode sensitivity")) {
            return slider;
        }
    }
    return nullptr;
}

int savedGgmorseSensitivity()
{
    return AppSettings::instance().value("CwDecoderSensitivity", "30").toString().toInt();
}

bool hasGgmorseThreshold(const PanadapterApplet& pan, int sensitivity)
{
    const float expected = 1.0f - (sensitivity / 100.0f) * 0.9f;
    return std::abs(pan.cwCostThreshold() - expected) < 1e-4f;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("cw-sens-pan-move"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Could not create the isolated settings profile\n");
        return 1;
    }

    QApplication app(argc, argv);
    AppSettings& settings = AppSettings::instance();
    settings.load();
    settings.setValue("CwDecoderSensitivity", "30");

    int failures = 0;
    const auto check = [&failures](bool condition, const char* description) {
        std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", description);
        if (!condition) {
            ++failures;
        }
    };

    PanadapterApplet outgoingPan;
    PanadapterApplet incomingPan;
    QSlider* outgoingSlider = sensitivitySlider(outgoingPan);
    QSlider* incomingSlider = sensitivitySlider(incomingPan);
    check(outgoingSlider && incomingSlider, "Both CW Sens sliders are present");
    if (!outgoingSlider || !incomingSlider) {
        return 1;
    }
    check(hasGgmorseThreshold(incomingPan, 30), "Incoming pan starts at saved Sens 30");

    outgoingSlider->setValue(80);
    outgoingPan.flushCwSens();
    check(savedGgmorseSensitivity() == 80, "Outgoing pan flushes pending Sens 80");
    incomingPan.restoreCwSens();
    check(incomingSlider->value() == 80, "Incoming slider shows saved Sens 80");
    check(hasGgmorseThreshold(incomingPan, 80), "Incoming threshold follows saved Sens 80");

    incomingSlider->setValue(40);
    emit incomingSlider->sliderReleased();
    // Process the outgoing slider's still-pending 500 ms debounce.
    QEventLoop debounceWindow;
    QTimer::singleShot(800, &debounceWindow, &QEventLoop::quit);
    debounceWindow.exec();
    check(savedGgmorseSensitivity() == 40, "Outgoing debounce preserves incoming Sens 40");

    incomingSlider->setValue(55);
    settings.setValue("CwDecoderSensitivity", "10");
    incomingPan.restoreCwSens();
    check(incomingSlider->value() == 55, "Restore preserves an unsaved slider edit");
    incomingPan.flushCwSens();

    settings.setValue("CwDecoderSensitivity", "250");
    outgoingPan.restoreCwSens();
    check(outgoingSlider->value() == 100 && hasGgmorseThreshold(outgoingPan, 100),
          "Out-of-range saved Sens clamps to 100");

#ifdef HAVE_CW_RX_BACKENDS
    settings.setValue("CwDecoderSensitivity", "60");
    CwDecodeSettings::setSensitivity(QStringLiteral("deepfist"), 50);
    incomingPan.setCwBackendState(QStringLiteral("deepfist"), false, {}, false, false, {});
    check(incomingSlider->value() == 50, "DeepFist slider shows its saved Sens 50");
    check(hasGgmorseThreshold(incomingPan, 60), "ggmorse keeps its own saved Sens with DeepFist selected");
#endif

    return failures == 0 ? 0 : 1;
}
