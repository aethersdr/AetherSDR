// The Phone/CW microphone Level gauge must remain empty until live telemetry
// arrives and must not carry a prior radio's reading across disconnect.

#include "TestSettingsProfile.h"
#include "gui/HGauge.h"
#include "gui/PhoneCwApplet.h"
#include "models/MeterModel.h"

#include <QApplication>
#include <QList>

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++failures;
    }
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("AETHER_AUTOMATION", "1");
    TestSettingsProfile profile(QStringLiteral("phone-cw-level-meter-state-test"));
    QApplication app(argc, argv);
    PhoneCwApplet applet;

    QWidget* levelWidget =
        applet.findChild<QWidget*>(QStringLiteral("phoneMicLevelGauge"));
    check(levelWidget != nullptr, "Phone/CW exposes the microphone Level gauge");
    if (!levelWidget) {
        return failures;
    }
    auto* levelGauge = static_cast<HGauge*>(levelWidget);

    check(levelGauge->value() == -40.0f,
          "the disconnected Level gauge starts at its floor");
    check(levelGauge->filledFraction() == 0.0f,
          "the disconnected Level gauge starts visually empty");

    applet.updateMeters(-12.0f, 0.0f, -8.0f, 0.0f);
    check(levelGauge->value() == -12.0f,
          "a live microphone sample reaches the Level gauge");

    applet.setMicLevelMeterState(MicMeterSessionState::Disconnected, false);
    check(!levelGauge->isHidden(),
          "disconnect keeps the permissive Level surface visible");
    check(levelGauge->value() == -40.0f,
          "disconnect clears the previous radio's Level reading");
    check(levelGauge->filledFraction() == 0.0f,
          "disconnect snaps the Level gauge back to empty");

    applet.setMicLevelMeterState(MicMeterSessionState::Connected, true);
    check(!levelGauge->isHidden(),
          "a later capable radio exposes the Level gauge");
    check(levelGauge->value() == -40.0f,
          "a later capable radio waits for its own microphone sample");

    applet.updateMeters(-18.0f, 0.0f, -14.0f, 0.0f);
    check(levelGauge->value() == -18.0f,
          "the later radio's own sample updates the Level gauge");

    applet.setMicLevelMeterState(MicMeterSessionState::Connected, false);
    check(levelGauge->isHidden(),
          "a connected radio without a microphone meter hides the gauge");
    check(levelGauge->value() == -40.0f,
          "hiding an unsupported gauge also clears its stale reading");

    applet.setMicLevelMeterState(MicMeterSessionState::Disconnected, false);
    applet.updateMeters(-16.0f, 0.0f, -12.0f, 0.0f);
    applet.setMicLevelMeterState(MicMeterSessionState::Disconnected, false);
    check(levelGauge->value() == -16.0f,
          "a repeated disconnected state preserves live PC-mic telemetry");

    // Drive the native ALC consumer, including active-slice invalidation.
    // There is no backend, transport or keyed transmitter.
    MeterModel meters;
    for (int slice = 0; slice < 2; ++slice) {
        MeterDef slc;
        slc.index = 10 + slice;
        slc.source = "SLC";
        slc.sourceIndex = slice;
        slc.name = "LEVEL";
        slc.unit = "dBm";
        meters.defineMeter(slc);
        MeterDef alc;
        alc.index = 20 + slice;
        alc.source = "TX-";
        alc.sourceIndex = 8 + slice;
        alc.name = "ALC";
        alc.unit = "Percent";
        meters.defineMeter(alc);
    }
    meters.setActiveTxSlice(1);
    QObject::connect(&meters, &MeterModel::alcValueChanged, &applet,
                     [&applet](float value, const QString& unit) {
        applet.setAlcMeterUnit(unit);
        if (unit.isEmpty()) {
            applet.resetAlc();
        } else {
            applet.updateAlc(value);
        }
    });
    QList<HGauge*> alcGauges;
    for (QWidget* widget : applet.findChildren<QWidget*>()) {
        if (widget->accessibleName() == "ALC gauge (Phone)"
            || widget->accessibleName() == "ALC gauge (CW)") {
            alcGauges.append(static_cast<HGauge*>(widget));
        }
    }
    check(alcGauges.size() == 2, "both Phone and CW ALC mirrors are present");
    const auto checkAlc = [&](float expected, const char* description) {
        for (const HGauge* gauge : alcGauges) {
            check(gauge->value() == expected, description);
        }
    };
    meters.updateValues({21}, {50});
    checkAlc(50.0f, "a percentage ALC sample reaches both gauges in native units");
    meters.setActiveTxSlice(0);
    checkAlc(0.0f, "changing TX slice sets both ALC gauges to empty");
    meters.updateValues({20}, {50});
    meters.removeMeter(20);
    checkAlc(0.0f, "active meter removal sets both ALC gauges to empty");

    return failures == 0 ? 0 : 1;
}
