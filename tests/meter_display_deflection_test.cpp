#include "models/MeterModel.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
using namespace AetherSDR;
namespace {
void require(bool condition, const char* claim)
{
    if (!condition) {
        std::cerr << claim << '\n';
        std::exit(1);
    }
}
}
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    MeterModel model;
    MeterDef def;
    def.index = 120;
    def.source = QStringLiteral("SLC");
    def.sourceIndex = 0;
    def.name = QStringLiteral("LEVEL");
    def.unit = QStringLiteral("dot");
    def.low = 0;
    def.high = 70;
    def.description = QStringLiteral("Radio display deflection; uncalibrated");
    int calibratedEvents = 0;
    QObject::connect(&model, &MeterModel::sLevelChanged, &model,
        [&](int, float) { ++calibratedEvents; });
    model.defineMeter(def);
    require(model.displayDeflectionForSlice(0) && !model.displayDeflectionForSlice(0)->value,
        "declared raw scale has no invented initial sample");
    require(!model.displayDeflectionForSlice(-1) && !model.displayDeflectionForSlice(1),
        "raw scale is associated with the physical receiver");
    for (float sample : {0.0f, 35.0f, 70.0f}) {
        require(model.updateValueByName("SLC", "LEVEL", sample, 0), "accept actual display sample");
        const std::optional<DisplayDeflectionReading> reading = model.displayDeflectionForSlice(0);
        require(reading && reading->value == sample && reading->unit == "dot"
            && reading->low == 0 && reading->high == 70 && reading->description == def.description
            && reading->sampleAgeMs >= 0 && reading->sampleAgeMs < MeterModel::kVitalsFreshMs,
            "typed reading preserves accepted native value, scale, units, description and freshness");
        require(!model.sLevelForSlice(0) && !model.sLevelIfLive() && !model.relativeLevelForSlice(0),
            "dots never enter calibrated dBm or ADC-relative dBFS APIs");
    }
    require(calibratedEvents == 0, "raw samples never emit calibrated S-level events");
    for (float invalid : {-1.0f, 71.0f}) {
        model.updateValueByName("SLC", "LEVEL", invalid, 0);
        require(!model.displayDeflectionForSlice(0)->value, "out-of-range raw samples are unavailable");
    }
    model.updateValueByName("SLC", "LEVEL", 35.0f, 0);
    require(!model.updateValueByName("SLC", "LEVEL", std::numeric_limits<float>::quiet_NaN(), 0),
        "non-finite telemetry is rejected at ingress");
    def.unavailableReason = "Radio is transmitting";
    model.defineMeter(def);
    require(!model.displayDeflectionForSlice(0)->value
        && model.displayDeflectionForSlice(0)->unavailableReason == def.unavailableReason,
        "radio withdrawal immediately masks a previously fresh sample");
    def.unavailableReason.clear();
    def.high = def.low;
    model.defineMeter(def);
    require(!model.displayDeflectionForSlice(0)->value, "degenerate raw bounds are unavailable");
    def.high = std::numeric_limits<double>::infinity();
    model.defineMeter(def);
    require(!model.displayDeflectionForSlice(0)->value, "non-finite raw bounds are unavailable");
    def.high = 70;
    model.defineMeter(def);
    model.updateValueByName("SLC", "LEVEL", 35.0f, 0);
    QEventLoop stale;
    QTimer::singleShot(MeterModel::kVitalsFreshMs + 10, &stale, &QEventLoop::quit);
    stale.exec();
    require(!model.displayDeflectionForSlice(0)->value, "stale raw sample parks the meter while retaining scale metadata");
    def.unit = "dBFS";
    model.defineMeter(def);
    require(!model.displayDeflectionForSlice(0) && !model.relativeLevelForSlice(0),
        "unit change cannot reinterpret an old dot sample");
    model.updateValueByName("SLC", "LEVEL", -35.0f, 0);
    require(model.relativeLevelForSlice(0) == -35.0f && !model.sLevelForSlice(0),
        "existing relative dBFS behavior is preserved");
    def.unit = "dBm";
    model.defineMeter(def);
    require(!model.sLevelForSlice(0), "calibrated unit change requires fresh calibrated telemetry");
    model.updateValueByName("SLC", "LEVEL", -73.0f, 0);
    require(model.sLevelForSlice(0) == -73.0f && calibratedEvents == 1,
        "existing calibrated dBm behavior is preserved");
    def.unit = "dot";
    model.defineMeter(def);
    model.updateValueByName("SLC", "LEVEL", 35.0f, 0);
    model.removeMeter(def.index);
    model.defineMeter(def);
    require(!model.displayDeflectionForSlice(0)->value, "recreated raw meter cannot inherit a retired sample");
    model.updateValueByName("SLC", "LEVEL", 35.0f, 0);
    model.clear();
    require(!model.displayDeflectionForSlice(0), "disconnect clears raw source and sample");
    std::cout << "Display-deflection model checks passed\n";
}
