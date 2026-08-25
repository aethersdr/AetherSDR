// Radio Vitals must distinguish an unsupported PA-temperature instrument from
// one that merely has not received its first sample. Capability changes hide
// and restore both the gauge and its unit selector as radio families change.

#include "gui/HGauge.h"
#include "gui/MeterApplet.h"
#include "models/MeterModel.h"
#include "TestSettingsProfile.h"

#include <QApplication>
#include <QPushButton>

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
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-meter-applet-capability-test"));
    QApplication app(argc, argv);
    MeterApplet applet;

    QWidget* paTempWidget =
        applet.findChild<QWidget*>(QStringLiteral("mtrPaTempGauge"));
    QPushButton* unitButton =
        applet.findChild<QPushButton*>(QStringLiteral("mtrTempUnitButton"));

    check(paTempWidget != nullptr, "Radio Vitals exposes a PA-temperature gauge");
    check(unitButton != nullptr, "Radio Vitals exposes the temperature-unit selector");
    if (!paTempWidget || !unitButton) {
        return failures;
    }
    auto* paTempGauge = static_cast<HGauge*>(paTempWidget);

    MeterModel model;
    MeterDef paTemp;
    paTemp.index = 1;
    paTemp.source = QStringLiteral("RAD");
    paTemp.name = QStringLiteral("PATEMP");
    paTemp.unit = QStringLiteral("degC");
    model.defineMeter(paTemp);
    applet.setMeterModel(&model);
    model.updateValues({1}, {static_cast<qint16>(55 * 64)});
    check(paTempGauge->value() == 55.0f,
          "a capable radio's live PA temperature reaches the gauge");
    check(paTempGauge->property("gaugeLabel").toString() == QStringLiteral("55.0°C"),
          "a capable radio's live PA temperature labels the gauge");

    applet.setPaTemperatureTelemetryState(true, false);
    check(paTempGauge->isHidden(),
          "an unavailable PA-temperature capability hides the gauge");
    check(unitButton->isHidden(),
          "an unavailable PA-temperature capability hides the unit selector");
    check(paTempGauge->value() == 0.0f,
          "hiding unsupported telemetry clears the previous radio's value");
    check(paTempGauge->property("gaugeLabel").toString() == QStringLiteral("PA Temp"),
          "hiding unsupported telemetry restores the neutral label");

    applet.setPaTemperatureTelemetryState(false, false);
    check(!paTempGauge->isHidden(),
          "disconnect restores the permissive PA-temperature surface");
    check(!unitButton->isHidden(),
          "disconnect restores the permissive unit selector");
    check(paTempGauge->value() == 0.0f,
          "disconnect does not restore the previous radio's temperature");

    applet.setPaTemperatureTelemetryState(true, true);
    check(!paTempGauge->isHidden(),
          "a later capable radio restores the PA-temperature gauge");
    check(paTempGauge->value() == 0.0f,
          "a later capable radio waits for its own temperature sample");
    check(paTempGauge->property("gaugeLabel").toString() == QStringLiteral("PA Temp"),
          "a later capable radio starts with the neutral label");

    model.updateValues({1}, {static_cast<qint16>(70 * 64)});
    check(paTempGauge->value() == 70.0f,
          "the later capable radio's own sample reaches the gauge");
    check(paTempGauge->property("gaugeLabel").toString() == QStringLiteral("70.0°C"),
          "the later capable radio's own sample labels the gauge");

    applet.setPaTemperatureTelemetryState(true, false);
    applet.setPaTemperatureTelemetryState(true, true);
    check(!paTempGauge->isHidden(),
          "re-enabling capability reveals the PA-temperature gauge");
    check(paTempGauge->value() == 0.0f,
          "re-enabled telemetry waits for a new sample instead of restoring stale data");
    check(paTempGauge->property("gaugeLabel").toString() == QStringLiteral("PA Temp"),
          "re-enabled telemetry restores the neutral label");

    return failures == 0 ? 0 : 1;
}
