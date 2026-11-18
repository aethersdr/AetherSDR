#include "core/txlinearity/TxLinearityTelemetry.h"
#include "models/MeterModel.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QThread>
#include <array>
#include <cstdio>
#include <memory>
#include <cmath>
#include <limits>

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
    const std::array<QString, 5> names = {"SWR", "PATEMP", "+13.8A", "FWDPWR", "REFPWR"};
    const std::array<QString, 5> units = {"SWR", "degC", "Volts", "dBm", "dBm"};
    // FLEX-8600 4.3.16: TX- num=1 FWDPWR, num=2 REFPWR, num=3 SWR.
    const std::array<int, 5> numbers = {3, 0, 0, 1, 2};
    for (int i = 0; i < 5; ++i) {
        MeterDef def;
        def.index = i + 1;
        def.source = (i == 0 || i >= 3) ? "TX-" : "RAD";
        def.sourceIndex = numbers[i];
        def.name = names[i];
        def.unit = units[i];
        model.defineMeter(def);
    }
}

void feed(MeterModel& model)
{
    model.updateValues({1, 2, 3, 4, 5}, {138, 2560, 3533, 4735, 1280});
}

void received(MeterModel& model, float forwardDbm, float reflectedDbm,
              ReceiveReceipt receipt, ReceiveDataLease session,
              ReceiveDataLease forwardDefinition = {}, ReceiveDataLease reflectedDefinition = {})
{
    if (!forwardDefinition) { forwardDefinition = std::make_shared<ReceiveDataEpoch>(); }
    if (!reflectedDefinition) { reflectedDefinition = std::make_shared<ReceiveDataEpoch>(); }
    model.updateReceivedValues({{{4, forwardDbm, forwardDefinition}, {5, reflectedDbm, reflectedDefinition}}, receipt, session});
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
    check(safety.evaluate(12701, reader.read(12701)) == AbortReason::None,
          "raw radio SWR is optional and never replaces the directional ratio");
    meters->removeMeter(5);
    check(safety.evaluate(12702, reader.read(12702)) == AbortReason::TelemetryStale,
          "removed reflected-power meter invalidates the pair");
    meters->clear();
    check(safety.evaluate(12800, reader.read(12800)) == AbortReason::TelemetryStale,
          "disconnect clearing cannot reuse previous samples");
    define(*meters);
    check(safety.evaluate(12900, reader.read(12900)) == AbortReason::TelemetryStale,
          "new definitions require new samples");
    meters.reset();
    check(safety.evaluate(13000, reader.read(13000)) == AbortReason::TelemetryStale,
          "destroyed model produces missing telemetry");

    {
        MeterModel model;
        define(model);
        TxLinearityTelemetry telemetry(&model);
        TxLinearitySafety guard;
        guard.noteRunStarted(10000);
        feed(model);
        for (const std::array<qint16, 3>& observed : {std::array<qint16, 3>{3586, 1014, 323},
                                                   std::array<qint16, 3>{3741, 1111, 294}}) {
            model.updateValues({4, 5, 1}, {observed[0], observed[1], observed[2]});
            const TxTelemetry t = telemetry.read(10000);
            check(t.swrSource == TxTelemetry::SwrSource::DirectionalPower
                && t.swr.value && *t.swr.value > 1.20 && *t.swr.value < 1.22
                && t.radioSwr.value == double(observed[2]) / 128,
                "preserved 5W/10W packets keep raw SWR while deriving the coherent ratio");
            check(guard.evaluate(10000, t) == AbortReason::None,
                  "actual conflicting radio-SWR packets no longer stop a safe directional ratio");
        }
        model.updateValues({4, 5, 1}, {3840, 3072, 128});
        check(guard.evaluate(10000, telemetry.read(10000)) == AbortReason::HighSwr,
              "high reflected power stops immediately even when raw radio SWR says unity");
        model.updateValues({4, 5}, {3840, 3840});
        check(guard.evaluate(10000, telemetry.read(10000)) == AbortReason::TelemetryStale,
              "unity reflection coefficient fails closed rather than clamping to a safe SWR");
        model.updateValues({4, 5}, {3840, 4000});
        check(guard.evaluate(10000, telemetry.read(10000)) == AbortReason::TelemetryStale,
              "reflected power above forward power fails closed");
        feed(model);
        model.updateValues({4}, {3840});
        model.updateValues({5}, {1280});
        check(!model.directionalPowerSample()
            && guard.evaluate(10000, telemetry.read(10000)) == AbortReason::TelemetryStale,
            "separate directional packets in the same millisecond cannot form a coherent pair");
        model.updateValues({4, 5, 4}, {3840, 1280, 3840});
        check(!model.directionalPowerSample(), "duplicate directional entries cannot establish a pair");
        MeterDef reflected = *model.meterDef(5);
        reflected.unit = "Counts";
        model.defineMeter(reflected);
        feed(model);
        check(!model.directionalPowerSample(), "unknown power units cannot establish a physical pair");
        reflected.unit = "dBm";
        reflected.source = "TX-other";
        model.defineMeter(reflected);
        feed(model);
        check(!model.directionalPowerSample(), "power from different meter sources cannot establish a pair");
        reflected.source = "TX-";
        model.defineMeter(reflected);
        feed(model);
        const TxTelemetry beforeRefresh = telemetry.read(10000);
        model.updateValues({1}, {323});
        const TxTelemetry rawRefresh = telemetry.read(12100);
        check(rawRefresh.swr.observedAtMs == beforeRefresh.swr.observedAtMs
            && guard.evaluate(12100, rawRefresh) == AbortReason::TelemetryStale,
            "new raw SWR cannot refresh the old directional pair");
    }
    {
        MeterModel model;
        define(model);
        feed(model);
        TxLinearityTelemetry telemetry(&model);
        TxLinearitySafety guard;
        guard.noteRunStarted(10000);
        auto session = std::make_shared<ReceiveDataEpoch>();
        auto definition = std::make_shared<ReceiveDataEpoch>();
        ReceiveReceipt receipt = ReceiveReceipt::now();
        received(model, 30.0f, 10.0f, receipt, session, definition, definition);
        check(guard.evaluate(10000, telemetry.read(10000)) == AbortReason::None,
              "accepted converted batch provides a coherent pair");
        definition->active.store(false);
        check(guard.evaluate(10000, telemetry.read(10000)) == AbortReason::TelemetryStale,
              "definition revocation withdraws a cached pair immediately");
        received(model, 30.0f, 10.0f, ReceiveReceipt::now(), session);
        session->active.store(false);
        check(guard.evaluate(10000, telemetry.read(10000)) == AbortReason::TelemetryStale,
              "session revocation withdraws a cached pair immediately");
        session = std::make_shared<ReceiveDataEpoch>();
        receipt = ReceiveReceipt::now();
        received(model, 30.0f, 10.0f, receipt, session);
        check(guard.evaluate(10000, telemetry.read(10000)) == AbortReason::None,
              "fresh pair is accepted after a new session");
        ++receipt.monotonicNs;
        received(model, 30.0f, 24.0f, receipt, session);
        check(guard.evaluate(10000, telemetry.read(10000)) == AbortReason::HighSwr,
              "new pair in the same UTC millisecond replaces the values");
        receipt.monotonicNs -= 2;
        received(model, 30.0f, 10.0f, receipt, session);
        check(guard.evaluate(10000, telemetry.read(10000)) == AbortReason::HighSwr,
              "reordered older packet cannot replace the newer high-reflection pair");
        receipt = ReceiveReceipt::now();
        receipt.monotonicNs -= 2100000000LL;
        // Start with a clean model so the old receipt is accepted, not reordered.
        model.clear(); define(model);
        received(model, 30.0f, 10.0f, receipt, session);
        check(telemetry.read(10000).swr.observedAtMs <= 7900,
              "delivery delay is charged to the original monotonic receipt");
        receipt = ReceiveReceipt::now();
        receipt.monotonicNs += 1000000000LL;
        received(model, 30.0f, 10.0f, receipt, session);
        check(telemetry.read(10000).swr.observedAtMs <= 7900,
              "rejected future receipt cannot renew the old pair");
        model.clear(); define(model); feed(model);
        const ReceiveReceipt negativeReceipt = ReceiveReceipt::now();
        MeterDef forward = *model.meterDef(4);
        MeterDef reflected = *model.meterDef(5);
        forward.unit = reflected.unit = "Watts";
        model.defineMeter(forward); model.defineMeter(reflected);
        received(model, 1.0f, -0.01f, negativeReceipt, session);
        check(guard.evaluate(10000, telemetry.read(10000)) == AbortReason::TelemetryStale,
              "negative physical power fails closed");
    }
    if (failures == 0) {
        std::puts("tx_linearity_telemetry_test: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
