// Real analog gauge rendering and accessibility; no radio/device/socket.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/SMeterWidget.h"
#include "models/MeterModel.h"
#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QTimer>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace AetherSDR;
namespace {
void require(bool condition, const char* claim)
{
    if (!condition) { std::cerr << claim << '\n'; std::exit(1); }
}
void spin(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
QImage render(SMeterWidget& widget)
{
    return widget.grab().toImage().scaled(widget.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
}
bool needleChanged(const SMeterWidget& widget, const QImage& first, const QImage& second, double fraction)
{
    const SMeterGeometry::MovementRay ray = widget.geometry().movementRayFor(widget.size(), fraction);
    const QPoint point = (ray.pivot + (ray.scalePoint - ray.pivot) * 0.87).toPoint();
    int changes = 0;
    for (int y = point.y() - 4; y <= point.y() + 4; ++y) {
        for (int x = point.x() - 4; x <= point.x() + 4; ++x) {
            if (first.rect().contains(x, y) && first.pixel(x, y) != second.pixel(x, y)) { ++changes; }
        }
    }
    return changes >= 3;
}
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("s-meter-display-deflection"));
    require(profile.isValid(), "isolated settings");
    QApplication application(argc, argv);
    AppSettings::instance().load();
    SMeterWidget meter;
    meter.resize(400, 180);
    meter.setRxMode(QStringLiteral("S-Meter"));
    meter.setPeakHoldEnabled(false);
    meter.show();
    DisplayDeflectionReading reading;
    reading.unit = "dot";
    reading.low = 0;
    reading.high = 70;
    reading.description = "Radio display deflection; uncalibrated";
    reading.value = 0;
    meter.setDisplayDeflection(reading, 0);
    spin(80);
    const QImage zero = render(meter);
    reading.value = 35;
    meter.setDisplayDeflection(reading, 0);
    spin(500);
    const QImage half = render(meter);
    require(meter.displayDeflection() && meter.relativeLevel() && std::isnan(meter.levelDbm()),
        "raw display cannot expose a calibrated level");
    require(meter.accessibleValueText().contains("35 dots")
        && !meter.accessibleValueText().contains("dBm") && !meter.accessibleValueText().contains("dBFS")
        && !meter.sUnitsText().startsWith('S'), "raw numeric and accessible readings use dots without S units");
    require(needleChanged(meter, zero, half, 0.5), "35 of 70 moves the real rendered needle to the middle of its arc");
    reading.value = 70;
    meter.setDisplayDeflection(reading, 0);
    spin(500);
    const QImage full = render(meter);
    require(needleChanged(meter, half, full, 1.0), "70 of 70 reaches the real rendered right endpoint");
    reading.value.reset();
    meter.setDisplayDeflection(reading, 0);
    const QImage stale = render(meter);
    require(meter.accessibleValueText().contains("unavailable") && std::isnan(meter.levelDbm()),
        "stale raw sample parks without fabricated calibrated value");
    require(!needleChanged(meter, zero, stale, 0.0), "stale raw needle parks at the same left endpoint as zero");
    reading.value = 35;
    meter.setDisplayDeflection(reading, 1);
    require(meter.accessibleValueText().contains("35 dots"), "receiver switch resets raw peak history");
    reading.high = reading.low;
    meter.setDisplayDeflection(reading, 1);
    require(meter.accessibleValueText().contains("unavailable"), "invalid raw bounds do not divide by zero");
    reading.high = 70;
    reading.value = std::numeric_limits<float>::quiet_NaN();
    meter.setDisplayDeflection(reading, 1);
    require(meter.accessibleValueText().contains("unavailable"), "non-finite raw reading cannot move needle");
    reading.value = 35;
    meter.setDisplayDeflection(reading, 1);
    meter.setTransmitting(true);
    meter.setTxMode("Power");
    meter.setTxMeters(25, 1.2);
    require(meter.accessibleValueText().contains("25 watts"), "raw RX support preserves the TX power face");
    meter.setDisplayDeflection(DisplayDeflectionReading{}, 1);
    require(meter.accessibleValueText().contains("25 watts"), "RX withdrawal during TX does not replace TX measurement");
    meter.setTransmitting(false);
    require(meter.accessibleValueText().contains("unavailable"), "unkey cannot resurrect retired RX dots");
    meter.setRelativeLevel(std::nullopt, 1);
    require(!meter.displayDeflection(), "unavailable dots can transition to unavailable dBFS");
    meter.setRelativeLevel(-35, 1);
    require(meter.accessibleValueText().contains("-35 dBFS"), "existing relative dBFS face is preserved");
    meter.setLevel(-73);
    require(!meter.displayDeflection() && !meter.relativeLevel() && meter.levelDbm() == -73
        && meter.accessibleValueText().contains("dBm"), "calibrated dBm face is restored without raw peak history");
    std::cout << "Analog display-deflection checks passed\n";
}
