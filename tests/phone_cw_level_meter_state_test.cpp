// Phone/CW gauges must not report what no radio has told them. The mic Level
// gauge must remain empty until live telemetry arrives and must not carry a
// prior radio's reading across disconnect; the ALC Gain gauge must not be on
// the shared panel at all for a radio that publishes no ALCGAIN meter.

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

    // The ALC Gain gauge answers the same question one step earlier: the
    // Phone panel is shared with Flex, Icom and the sim, none of which
    // publish TX:ALCGAIN, so the row must not exist for them at all.
    QWidget* alcGainWidget =
        applet.findChild<QWidget*>(QStringLiteral("phoneAlcGainGauge"));
    check(alcGainWidget != nullptr, "Phone exposes the ALC Gain gauge");
    if (alcGainWidget) {
        auto* alcGainGauge = static_cast<HGauge*>(alcGainWidget);
        check(alcGainGauge->isHidden(),
              "a radio that has not declared an ALCGAIN meter shows no ALC Gain row");
        applet.setHasAlcGainMeter(true);
        check(!alcGainGauge->isHidden(),
              "declaring an ALCGAIN meter reveals the ALC Gain row");
        applet.updateAlcGain(12.0f);
        check(alcGainGauge->value() == 12.0f,
              "a gain sample reaches the revealed ALC Gain gauge");
        applet.setHasAlcGainMeter(false);
        check(alcGainGauge->isHidden(),
              "a radio without the meter takes the ALC Gain row back down");
        // THE FLOOR, NOT ZERO. This assertion said 0.0f and passed, which
        // pinned the defect as correct behaviour: on a -20..+40 face, 0 dB
        // renders as a bar one third full AND is a real reading ("the ALC is
        // holding at unity"), so the cleared state was drawn as a confident
        // measurement. An empty bar is the only rendering of "no reading" this
        // widget has.
        check(alcGainGauge->value() == -20.0f,
              "hiding the ALC Gain row empties the bar rather than parking it "
              "at a readable 0 dB");
    }

    // The same via the model accessor the GUI actually gates on.
    check(!meters.hasAlcGainMeter(),
          "a radio publishing ALC but not ALCGAIN has no ALC Gain meter");
    MeterDef alcGain;
    alcGain.index = 30;
    alcGain.source = "TX-";
    alcGain.sourceIndex = 8;
    alcGain.name = "ALCGAIN";
    alcGain.unit = "dB";
    meters.defineMeter(alcGain);
    check(meters.hasAlcGainMeter(),
          "defining ALCGAIN is what makes the meter present");
    meters.clear();
    check(!meters.hasAlcGainMeter(),
          "disconnect takes the ALCGAIN meter away with the radio");

    return failures == 0 ? 0 : 1;
}
