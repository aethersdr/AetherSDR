#include "core/txlinearity/TxLinearityTelemetry.h"
#include "models/MeterModel.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QThread>
#include <array>
#include <cstdio>
#include <memory>

using namespace AetherSDR;
using namespace AetherSDR::txlin;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void define(MeterModel& model)
{
    const std::array<QString, 4> names = {"SWR", "PATEMP", "+13.8A", "FWDPWR"};
    const std::array<QString, 4> units = {"SWR", "degC", "Volts", "Watts"};
    for (int i = 0; i < 4; ++i) {
        MeterDef def;
        def.index = i + 1;
        def.source = (i == 0 || i == 3) ? "TX" : "RAD";
        def.name = names[i];
        def.unit = units[i];
        model.defineMeter(def);
    }
}

void feed(MeterModel& model)
{
    check(model.updateValueByName("TX", "SWR", 1.08f), "feed SWR");
    check(model.updateValueByName("RAD", "PATEMP", 40.0f), "feed temperature");
    check(model.updateValueByName("RAD", "+13.8A", 13.8f), "feed voltage");
    check(model.updateValueByName("TX", "FWDPWR", 5.0f), "feed power");
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    auto meters = std::make_unique<MeterModel>();
    define(*meters);
    TxLinearityTelemetry reader(meters.get());
    TxLinearitySafety safety;
    safety.noteRunStarted(10000);
    check(safety.evaluate(10000, reader.read(10000)) == AbortReason::TelemetryStale,
          "definitions alone cannot satisfy the guard");
    feed(*meters);
    const TxTelemetry initial = reader.read(10000);
    check(safety.evaluate(10000, initial) == AbortReason::None,
          "actual model samples satisfy the guard");
    for (std::int64_t now = 12100; now <= 12500; now += 100) {
        const TxTelemetry polled = reader.read(now);
        check(polled.paTempC.observedAtMs == initial.paTempC.observedAtMs,
              "polling preserves the received timestamp");
        check(safety.evaluate(now, polled) == AbortReason::TelemetryStale,
              "cached real-model readings expire despite repeated polls");
    }
    // A new unchanged-value sample must have a distinct millisecond stamp.
    const qint64 priorStamp = meters->paTempUpdatedAtMs();
    while (QDateTime::currentMSecsSinceEpoch() <= priorStamp) {
        QThread::msleep(1);
    }
    check(meters->updateValueByName("RAD", "PATEMP", 40.0f), "refresh temperature only");
    const TxTelemetry partial = reader.read(12600);
    check(partial.paTempC.observedAtMs > initial.paTempC.observedAtMs,
          "unchanged temperature with a new packet refreshes its sample time");
    check(safety.evaluate(12600, partial) == AbortReason::TelemetryStale,
          "fresh temperature cannot conceal stale directional power and voltage");
    feed(*meters);
    check(safety.evaluate(12700, reader.read(12700)) == AbortReason::None,
          "new unchanged samples for every meter restore freshness");
    meters->removeMeter(1);
    check(safety.evaluate(12701, reader.read(12701)) == AbortReason::TelemetryStale,
          "removed SWR cannot leave a valid cached ratio");
    meters->clear();
    check(safety.evaluate(12800, reader.read(12800)) == AbortReason::TelemetryStale,
          "disconnect clearing cannot reuse previous samples");
    define(*meters);
    check(safety.evaluate(12900, reader.read(12900)) == AbortReason::TelemetryStale,
          "new definitions require new samples");
    meters.reset();
    check(safety.evaluate(13000, reader.read(13000)) == AbortReason::TelemetryStale,
          "destroyed model produces missing telemetry");
    if (failures == 0) {
        std::puts("tx_linearity_telemetry_test: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
