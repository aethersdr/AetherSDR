// Radio Vitals must not present a nominal or zero supply voltage before the
// connected radio has supplied a real telemetry sample.

#include "gui/HGauge.h"
#include "gui/MeterApplet.h"
#include "models/MeterModel.h"
#include "TestSettingsProfile.h"

#include <QApplication>
#include <QFile>

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
        QStringLiteral("aether-meter-applet-voltage-state-test"));
    QApplication app(argc, argv);
    MeterApplet applet;

    QWidget* supplyWidget =
        applet.findChild<QWidget*>(QStringLiteral("mtrSupplyVoltageGauge"));
    check(supplyWidget != nullptr, "Radio Vitals exposes a supply-voltage gauge");
    if (!supplyWidget) {
        return failures;
    }
    auto* supplyGauge = static_cast<HGauge*>(supplyWidget);

    check(supplyGauge->property("gaugeLabel").toString()
              == QStringLiteral("Supply Voltage"),
          "the disconnected gauge starts with a neutral label");

    MeterModel model;
    MeterDef paTemp;
    paTemp.index = 1;
    paTemp.source = QStringLiteral("RAD");
    paTemp.name = QStringLiteral("PATEMP");
    paTemp.unit = QStringLiteral("degC");
    model.defineMeter(paTemp);
    applet.setMeterModel(&model);

    model.updateValues({1}, {static_cast<qint16>(55 * 64)});
    check(supplyGauge->property("gaugeLabel").toString()
              == QStringLiteral("Supply Voltage"),
          "unrelated hardware telemetry does not fabricate a voltage");

    MeterDef supply;
    supply.index = 2;
    supply.source = QStringLiteral("RAD");
    supply.name = QStringLiteral("+13.8A");
    supply.unit = QStringLiteral("Volts");
    model.defineMeter(supply);
    model.updateValues({2}, {static_cast<qint16>(13.5f * 256.0f)});
    check(model.hasSupplyVoltage(), "the model records receipt of a voltage sample");
    check(supplyGauge->property("gaugeLabel").toString()
              == QStringLiteral("+13.50V"),
          "a real telemetry sample replaces the neutral label");

    applet.setSupplyVoltageTelemetryState(true, true);
    check(supplyGauge->property("gaugeLabel").toString()
              == QStringLiteral("+13.50V"),
          "a live sample survives a mid-session capability republish");

    model.removeMeter(2);
    applet.setSupplyVoltageTelemetryState(true, true);
    check(supplyGauge->property("gaugeLabel").toString()
              == QStringLiteral("Supply Voltage"),
          "removing the voltage meter clears its stale reading");

    model.defineMeter(supply);
    model.updateValues({2}, {static_cast<qint16>(13.5f * 256.0f)});
    model.clear();
    applet.setSupplyVoltageTelemetryState(false, true);
    check(supplyGauge->property("gaugeLabel").toString()
              == QStringLiteral("Supply Voltage"),
          "disconnect clears the previous radio's voltage");
    check(supplyGauge->value() == 0.0f,
          "disconnect clears the previous radio's gauge value");

    applet.setSupplyVoltageTelemetryState(true, true);
    check(supplyGauge->property("gaugeLabel").toString()
              == QStringLiteral("Supply Voltage"),
          "a new session waits for its own telemetry sample");

    // The state call drops a held reading on a radio with no supply rail; it
    // never hides the gauge (dimming is the registry's job, see
    // meter_applet_supply_availability_test).
    model.defineMeter(supply);
    model.updateValues({2}, {static_cast<qint16>(13.5f * 256.0f)});
    check(supplyGauge->property("gaugeLabel").toString()
              == QStringLiteral("+13.50V"),
          "precondition: the gauge holds a reading");
    applet.setSupplyVoltageTelemetryState(true, false);
    check(!supplyGauge->isHidden(),
          "a radio that reports no supply rail (HL2) does not hide the gauge");
    check(supplyGauge->property("gaugeLabel").toString()
              == QStringLiteral("Supply Voltage")
              && supplyGauge->value() == 0.0f,
          "and drops any reading it held");

    QFile mainWindowSource(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow.cpp"));
    check(mainWindowSource.open(QIODevice::ReadOnly)
              && mainWindowSource.readAll().contains(
                  "setSupplyVoltageTelemetryState(\n"
                  "            connected, caps.hasSupplyVoltageTelemetry)"),
          "MainWindow passes the radio's supply-telemetry capability to the applet");

    return failures == 0 ? 0 : 1;
}
