// Radio Vitals must distinguish an unsupported PA-temperature instrument from
// one that merely has not received its first sample. Capability changes hide
// and restore both the gauge and its unit selector as radio families change.

#include "gui/HGauge.h"
#include "gui/MeterApplet.h"
#include "models/MeterModel.h"
#include "TestSettingsProfile.h"

#include <QApplication>
#include <QAccessible>
#include <QFile>
#include <QPushButton>

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;
QObject* g_nameChangedObject = nullptr;

void captureAccessibleNameUpdate(QAccessibleEvent* event)
{
    if (event->type() == QAccessible::NameChanged) {
        g_nameChangedObject = event->object();
    }
}

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
    applet.setPaInstrumentTelemetryState(true, true, false);
    model.updateValues({1}, {static_cast<qint16>(55 * 64)});
    check(paTempGauge->value() == 55.0f,
          "a capable radio's live PA temperature reaches the gauge");
    check(paTempGauge->property("gaugeLabel").toString() == QStringLiteral("55.0°C"),
          "a capable radio's live PA temperature labels the gauge");

    applet.setPaInstrumentTelemetryState(true, false, false);
    check(paTempGauge->isHidden(),
          "an unavailable PA-temperature capability hides the gauge");
    check(unitButton->isHidden(),
          "an unavailable PA-temperature capability hides the unit selector");
    check(paTempGauge->value() == 0.0f,
          "hiding unsupported telemetry clears the previous radio's value");
    check(paTempGauge->property("gaugeLabel").toString() == QStringLiteral("PA Temp"),
          "hiding unsupported telemetry restores the neutral label");

    applet.setPaInstrumentTelemetryState(false, false, false);
    check(!paTempGauge->isHidden(),
          "disconnect restores the permissive PA-temperature surface");
    check(!unitButton->isHidden(),
          "disconnect restores the permissive unit selector");
    check(paTempGauge->value() == 0.0f,
          "disconnect does not restore the previous radio's temperature");

    applet.setPaInstrumentTelemetryState(true, true, false);
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

    applet.setPaInstrumentTelemetryState(true, false, false);
    applet.setPaInstrumentTelemetryState(true, true, false);
    check(!paTempGauge->isHidden(),
          "re-enabling capability reveals the PA-temperature gauge");
    check(paTempGauge->value() == 0.0f,
          "re-enabled telemetry waits for a new sample instead of restoring stale data");
    check(paTempGauge->property("gaugeLabel").toString() == QStringLiteral("PA Temp"),
          "re-enabled telemetry restores the neutral label");

    MeterDef paCurrent;
    paCurrent.index = 2;
    paCurrent.source = QStringLiteral("RAD");
    paCurrent.name = QStringLiteral("PACURRENT");
    paCurrent.unit = QStringLiteral("Amps");
    paCurrent.low = 0.0;
    paCurrent.high = 20.0;
    model.defineMeter(paCurrent);

    const QAccessible::UpdateHandler previousHandler =
        QAccessible::installUpdateHandler(captureAccessibleNameUpdate);
    const bool wasAccessible = QAccessible::isActive();
    QAccessible::setActive(true);
    g_nameChangedObject = nullptr;
    applet.setPaInstrumentTelemetryState(true, false, true);
    check(!paTempGauge->isHidden(),
          "PA-current capability reuses the unavailable temperature row");
    check(unitButton->isHidden(),
          "the temperature-unit selector stays hidden for PA current");
    check(paTempGauge->property("gaugeLabel").toString()
              == QStringLiteral("PA Current"),
          "PA-current capability starts with a neutral label");
    check(paTempGauge->accessibleName() == QStringLiteral("PA drain current"),
          "PA-current capability exposes the selected instrument to assistive technology");
    if (QAccessible::isActive()) {
        check(g_nameChangedObject == paTempGauge,
              "changing instruments emits NameChanged for the reused gauge");
    }
    QAccessible::installUpdateHandler(previousHandler);
    QAccessible::setActive(wasAccessible);
    check(paTempGauge->property("gaugeYellowStart").toFloat() == 18.0f
              && paTempGauge->property("gaugeRedStart").toFloat() == 20.0f,
          "the IC-9700 warns at its documented 18 A maximum and redlines at full scale");

    applet.setTransmitting(true);
    model.updateValues({2}, {static_cast<qint16>(10 * 256)});
    check(paTempGauge->value() == 10.0f,
          "a calibrated PA-current sample reaches the reused gauge");
    check(paTempGauge->property("gaugeLabel").toString()
              == QStringLiteral("10.0 A"),
          "the PA-current sample shows the value and amperes without a redundant prefix");

    applet.setPaInstrumentTelemetryState(true, false, true);
    check(paTempGauge->value() == 10.0f,
          "an identical capability refresh preserves the live PA-current sample");
    check(paTempGauge->property("gaugeLabel").toString()
              == QStringLiteral("10.0 A"),
          "an identical capability refresh does not flash the neutral label");

    applet.setTransmitting(false);
    check(paTempGauge->value() == 0.0f,
          "unkey clears the PA-current gauge immediately");
    check(paTempGauge->property("gaugeLabel").toString()
              == QStringLiteral("PA Current"),
          "unkey restores the neutral PA-current label");

    model.updateValues({2}, {static_cast<qint16>(8 * 256)});
    check(paTempGauge->value() == 0.0f,
          "a late PA-current reply after unkey cannot restore a stale reading");

    // Temperature has priority when a future backend honestly declares both
    // instruments. The current capability is a fallback face, not a reason to
    // suppress or overwrite the preferred temperature reading.
    applet.setPaInstrumentTelemetryState(true, true, true);
    model.updateValues({2}, {static_cast<qint16>(12 * 256)});
    model.updateValues({1}, {static_cast<qint16>(61 * 64)});
    check(!unitButton->isHidden(),
          "temperature/current coexistence keeps the temperature-unit selector");
    check(paTempGauge->property("gaugeLabel").toString()
              == QStringLiteral("61.0°C"),
          "PA temperature wins when both telemetry capabilities are declared");
    check(paTempGauge->accessibleName() == QStringLiteral("PA temperature"),
          "temperature priority restores the accessible instrument name");
    model.updateValues({2}, {static_cast<qint16>(14 * 256)});
    check(paTempGauge->property("gaugeLabel").toString()
              == QStringLiteral("61.0°C"),
          "PA current cannot overwrite the preferred temperature instrument");

    applet.setPaInstrumentTelemetryState(false, false, false);
    check(!paTempGauge->isHidden(),
          "disconnect restores the permissive PA-temperature surface after current");
    check(paTempGauge->property("gaugeLabel").toString() == QStringLiteral("PA Temp"),
          "disconnect clears the previous radio's PA-current sample");

    // Pin both production fan-outs. This focused target intentionally does not
    // link MainWindow; without these guards, deleting either
    // connection leaves all widget-level tests green while the shipping app
    // never selects the current face or clears it at unkey.
    QFile mainWindowSource(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow.cpp"));
    check(mainWindowSource.open(QIODevice::ReadOnly),
          "the capability test can inspect MainWindow's shipping fan-out");
    const QByteArray capabilityWiring = mainWindowSource.readAll();
    check(capabilityWiring.contains("setPaInstrumentTelemetryState("),
          "MainWindow wires normalized PA-instrument capabilities to Radio Vitals");
    check(capabilityWiring.contains("caps.hasPaCurrentTelemetry"),
          "MainWindow includes PA-current capability in that atomic update");

    QFile meterAppletSource(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MeterApplet.cpp"));
    check(meterAppletSource.open(QIODevice::ReadOnly),
          "the capability test can inspect the accessible-name implementation");
    check(meterAppletSource.readAll().contains("QAccessible::NameChanged"),
          "dynamic PA-instrument names notify assistive technology");

    QFile sessionSource(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow_Session.cpp"));
    check(sessionSource.open(QIODevice::ReadOnly),
          "the capability test can inspect the shipping radio-TX fan-out");
    const QByteArray txWiring = sessionSource.readAll();
    check(txWiring.contains("meterApplet()->setTransmitting(tx);"),
          "authoritative radio-TX edges drive the Radio Vitals current face");

    QFile appletPanelSource(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/AppletPanel.cpp"));
    check(appletPanelSource.open(QIODevice::ReadOnly),
          "the capability test can inspect the optimistic meter fan-out");
    check(!appletPanelSource.readAll().contains(
              "m_meterApplet->setTransmitting(transmitting);"),
          "optimistic MOX and direct-amplifier edges cannot clear PA current early");

    return failures == 0 ? 0 : 1;
}
