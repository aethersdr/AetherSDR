// The MTR applet's Supply Voltage gauge on a radio that declares no supply
// telemetry (the Hermes-Lite 2): dimmed with a stated reason, never hidden
// (docs/agents/gui.md, theme-style-guide §4a).
//
// Socket-free: a bare RadioModel; capabilities are pushed through its signal.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/ControlAvailabilityRegistry.h"
#include "gui/HGauge.h"
#include "gui/MeterApplet.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QFile>
#include <QImage>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

// How far the gauge's pixels stand out from the background it is drawn on
// (the top-left pixel lies above the bar). Dimming lowers this whether the
// background is light or dark.
static qint64 contrast(QWidget* w)
{
    const QImage img = w->grab().toImage();
    const QColor bg = img.pixelColor(0, 0);
    qint64 sum = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            sum += qAbs(c.red() - bg.red()) + qAbs(c.green() - bg.green())
                 + qAbs(c.blue() - bg.blue());
        }
    }
    return sum;
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("meter-supply-availability"));
    QApplication app(argc, argv);
    AppSettings::instance().load();

    RadioModel model;
    MeterApplet applet;
    // The registration MainWindow_Wiring makes, with the applet's own predicate
    // and reason (checked against that source below).
    ControlAvailabilityRegistry registry(model);
    registry.registerWidget(applet.supplyGauge(),
        MeterApplet::supplyVoltageUnavailableReason(),
        &MeterApplet::supplyVoltageAvailable);
    HGauge* gauge = applet.supplyGauge();
    check(gauge != nullptr
              && gauge->objectName() == QStringLiteral("mtrSupplyVoltageGauge"),
          "the applet hands out its Supply Voltage gauge");
    if (!gauge) {
        return 1;
    }
    gauge->resize(200, 30);
    gauge->setLabel(QStringLiteral("+13.50V"));
    gauge->setValueImmediate(13.5f);

    check(gauge->isEnabled(), "disconnected: the gauge is not dimmed");

    RadioCapabilities noRail;
    noRail.hasSupplyVoltageTelemetry = false;
    RadioCapabilities withRail;
    withRail.hasSupplyVoltageTelemetry = true;

    const qint64 lit = contrast(gauge);
    emit model.capabilitiesChanged(true, noRail);
    check(!gauge->isEnabled(), "a radio with no supply rail (HL2) dims the gauge");
    check(!gauge->isHidden(), "and does not hide it");
    check(gauge->toolTip().contains(QStringLiteral("supply voltage")),
          "the reason is the tooltip");
    check(gauge->accessibleDescription().contains(QStringLiteral("supply voltage")),
          "and reaches a screen reader as the accessible description");
    check(contrast(gauge) < lit * 6 / 10, "a dimmed gauge is drawn visibly fainter");

    emit model.capabilitiesChanged(true, withRail);
    check(gauge->isEnabled() && gauge->toolTip().isEmpty(),
          "a radio that reports the rail (Flex, ANAN) shows it undimmed, no reason");
    check(contrast(gauge) == lit, "and draws it as before");

    emit model.capabilitiesChanged(true, noRail);
    emit model.capabilitiesChanged(false, noRail);
    check(gauge->isEnabled(), "disconnect lifts the dimming");

    QFile wiring(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow_Wiring.cpp"));
    const QByteArray src = wiring.open(QIODevice::ReadOnly) ? wiring.readAll() : QByteArray();
    check(src.contains("registerWidget(m_appletPanel->meterApplet()->supplyGauge(),")
              && src.contains("MeterApplet::supplyVoltageUnavailableReason()")
              && src.contains("&MeterApplet::supplyVoltageAvailable"),
          "MainWindow registers the gauge with that predicate and reason");

    return g_failures == 0 ? 0 : 1;
}
