// The MTR applet's Supply Voltage gauge on a radio that declares no supply
// telemetry (the Hermes-Lite 2): dimmed with a stated reason, never hidden
// (docs/agents/gui.md, theme-style-guide §4a).
//
// Socket-free: a bare RadioModel; capabilities are pushed through its signal.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/icom/IcomCivBackend.h"
#include "gui/ControlAvailabilityRegistry.h"
#include "gui/HGauge.h"
#include "gui/MeterApplet.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QFile>
#include <QImage>
#include <QStringList>

#include <cstdio>
#include <initializer_list>

using namespace AetherSDR;

namespace AetherSDR::icom {
struct IcomCivBackendTestAccess {
    static void selectModel(IcomCivBackend& backend, const IcomModel& model)
    {
        backend.m_model = &model;
    }
};
}

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
    qputenv("AETHER_AUTOMATION", "1");
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

    // Use the production factory without connecting any transport or device.
    // Switching from an unsupported family to a supported one must restore
    // the original pixels, reading and help text, regardless of family order.
    const QStringList families = {QStringLiteral("hl2"), QStringLiteral("flex"),
        QStringLiteral("icom"), QStringLiteral("anan"), QStringLiteral("rtl"),
        QStringLiteral("sim"), QStringLiteral("flex")};
    for (const QString& family : families) {
#ifndef AETHER_BACKEND_RTL
        if (family == QStringLiteral("rtl")) {
            std::printf("-- rtl: not built, skipped\n");
            continue;
        }
#endif
        std::printf("-- cold backend: %s\n", qPrintable(family));
        check(model.rebuildBackendForTest(family), "the production backend is constructed");
        const RadioCapabilities caps = model.backendCapabilities();
        check(caps.family == family, "the capability payload belongs to this family");
        const bool expected = family == QStringLiteral("flex")
            || family == QStringLiteral("anan");
        check(caps.hasSupplyVoltageTelemetry == expected,
              "the cold backend declares its expected supply telemetry");
        emit model.capabilitiesChanged(true, caps);
        check(gauge->isEnabled() == expected && !gauge->isHidden(),
              "availability follows the production capability without hiding");
        check(expected ? contrast(gauge) == lit : contrast(gauge) < lit * 6 / 10,
              "supported families retain the original rendering after a family switch");
        check(gauge->value() == 13.5f
                  && gauge->property("gaugeLabel").toString() == QStringLiteral("+13.50V"),
              "availability does not alter the gauge's telemetry value");
        check(expected ? gauge->toolTip().isEmpty()
                       : gauge->toolTip() == MeterApplet::supplyVoltageUnavailableReason(),
              "supported families recover their original tooltip");
        emit model.capabilitiesChanged(false, caps);
        check(gauge->isEnabled(), "disconnect restores the gauge for the next family");
    }

    // Selecting an existing model profile exercises the calibrated Icom
    // capability path without starting a session or sending CI-V commands.
    icom::IcomCivBackend icomBackend;
    for (const char* name : {"IC-705", "IC-9700", "IC-7300MK2", "IC-7300"}) {
        std::printf("-- cold Icom profile: %s\n", name);
        const icom::IcomModel* profileModel = icom::modelForName(name);
        check(profileModel != nullptr, "the Icom model profile exists");
        if (!profileModel) {
            continue;
        }
        icom::IcomCivBackendTestAccess::selectModel(icomBackend, *profileModel);
        const RadioCapabilities caps = icomBackend.capabilities();
        const bool expected = profileModel->name != "IC-7300";
        check(caps.hasSupplyVoltageTelemetry == expected,
              "only calibrated Icom profiles declare supply telemetry");
        emit model.capabilitiesChanged(true, caps);
        check(gauge->isEnabled() == expected && !gauge->isHidden(),
              "Icom model availability follows its own profile");
        check(expected ? contrast(gauge) == lit : contrast(gauge) < lit * 6 / 10,
              "calibrated Icom models retain the original rendering");
    }

    QFile wiring(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow_Wiring.cpp"));
    const QByteArray src = wiring.open(QIODevice::ReadOnly) ? wiring.readAll() : QByteArray();
    check(src.contains("registerWidget(m_appletPanel->meterApplet()->supplyGauge(),")
              && src.contains("MeterApplet::supplyVoltageUnavailableReason()")
              && src.contains("&MeterApplet::supplyVoltageAvailable"),
          "MainWindow registers the gauge with that predicate and reason");

    return g_failures == 0 ? 0 : 1;
}
