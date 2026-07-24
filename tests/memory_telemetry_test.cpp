#include "core/MemoryTelemetry.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

#include <cmath>
#include <cstdio>

using AetherSDR::MemoryTelemetrySeries;
using AetherSDR::ProcessMemorySnapshot;

namespace {

int g_failures = 0;

void check(const char* name, bool condition)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", name);
    if (!condition) {
        ++g_failures;
    }
}

QJsonObject snapshot(double residentBytes, double panBytes, int spectrumWidgets)
{
    return QJsonObject{
        {QStringLiteral("process"), QJsonObject{
            {QStringLiteral("residentBytes"), residentBytes},
            {QStringLiteral("peakResidentBytes"), residentBytes},
            {QStringLiteral("privateBytes"), residentBytes * 0.8},
            {QStringLiteral("virtualBytes"), residentBytes * 2.0},
            {QStringLiteral("allocatorInUseBytes"), residentBytes * 0.5},
            {QStringLiteral("allocatorReservedBytes"), residentBytes * 0.6},
            {QStringLiteral("threadCount"), 10},
        }},
        {QStringLiteral("subsystems"), QJsonObject{
            {QStringLiteral("panadapter"), QJsonObject{
                {QStringLiteral("trackedBytes"), panBytes},
                {QStringLiteral("estimatedGpuBytes"), panBytes * 0.25},
                {QStringLiteral("objectCount"), spectrumWidgets * 3},
                {QStringLiteral("classes"), QJsonObject{
                    {QStringLiteral("SpectrumWidget"), spectrumWidgets},
                    {QStringLiteral("QTimer"), spectrumWidgets},
                }},
            }},
        }},
    };
}

void testLinearGrowth()
{
    MemoryTelemetrySeries series;
    constexpr double mebibyte = 1024.0 * 1024.0;
    for (int i = 0; i < 6; ++i) {
        series.addSnapshot(i * 60000LL,
                           snapshot((100.0 + i * 5.0) * mebibyte,
                                    (20.0 + i * 2.0) * mebibyte,
                                    1 + i));
    }

    const QJsonObject report = series.report(false);
    const QJsonObject trend = report.value(QStringLiteral("byteTrends"))
        .toObject().value(QStringLiteral("process.residentBytes")).toObject();
    check("linear series retains all samples", series.sampleCount() == 6);
    check("linear resident growth is flagged",
          trend.value(QStringLiteral("sustainedGrowth")).toBool());
    check("linear fit has R squared near one",
          trend.value(QStringLiteral("rSquared")).toDouble() > 0.999);
    check("resident slope is 300 MiB/hour",
          std::abs(trend.value(QStringLiteral("slopeBytesPerHour")).toDouble()
                   - 300.0 * mebibyte) < 1.0);

    const QJsonObject classGrowth = report.value(QStringLiteral("classCountGrowth"))
        .toObject().value(QStringLiteral("panadapter")).toObject();
    check("class growth reports five retained SpectrumWidgets",
          classGrowth.value(QStringLiteral("SpectrumWidget")).toInt() == 5);
    check("growth suspect includes process and subsystem metrics",
          report.value(QStringLiteral("growthSuspects")).toArray().size() >= 2);
}

void testFlatSeries()
{
    MemoryTelemetrySeries series;
    for (int i = 0; i < 8; ++i) {
        series.addSnapshot(i * 60000LL, snapshot(100000000.0, 20000000.0, 1));
    }
    const QJsonObject report = series.report(false);
    const QJsonObject resident = report.value(QStringLiteral("byteTrends"))
        .toObject().value(QStringLiteral("process.residentBytes")).toObject();
    check("flat resident series is not flagged",
          !resident.value(QStringLiteral("sustainedGrowth")).toBool());
    check("flat series has zero delta",
          resident.value(QStringLiteral("delta")).toDouble() == 0.0);
    check("flat class inventory has no growth entries",
          report.value(QStringLiteral("classCountGrowth")).toObject().isEmpty());
}

void testBoundedRetention()
{
    MemoryTelemetrySeries series(3);
    for (int i = 0; i < 5; ++i) {
        series.addSnapshot(i * 1000LL, snapshot(1000.0 + i, 100.0, 1));
    }
    const QJsonObject report = series.report(true);
    check("sample retention is bounded", series.sampleCount() == 3);
    check("raw sample export matches retained count",
          report.value(QStringLiteral("samples")).toArray().size() == 3);
    check("retained series duration uses oldest retained point",
          report.value(QStringLiteral("durationMs")).toInt() == 2000);
}

void testLateMetricDurationGate()
{
    MemoryTelemetrySeries series;
    constexpr double mebibyte = 1024.0 * 1024.0;
    // Early samples with NO panadapter subsystem, spanning well over a minute.
    for (int i = 0; i < 3; ++i) {
        series.addSnapshot(i * 400000LL, QJsonObject{
            {QStringLiteral("process"), QJsonObject{
                {QStringLiteral("residentBytes"), 100.0 * mebibyte},
            }},
        });
    }
    // Six panadapter samples that appear only late and span 50s (< the 60s
    // sustained-growth duration gate), but grow hard (steep slope, R^2 ~ 1).
    const qint64 base = 1200000LL;
    for (int i = 0; i < 6; ++i) {
        series.addSnapshot(base + i * 10000LL,
                           snapshot(100.0 * mebibyte,
                                    (100.0 + i * 20.0) * mebibyte, 1 + i));
    }
    const QJsonObject trend = series.report(false)
        .value(QStringLiteral("byteTrends")).toObject()
        .value(QStringLiteral("subsystem.panadapter.trackedBytes")).toObject();
    check("late-appearing metric collects its six samples",
          trend.value(QStringLiteral("samples")).toInt() == 6);
    // The metric's OWN span is 50s, so despite the steep growth it must not be
    // flagged sustained — the duration gate uses the metric's span, not the
    // full series range (which here is ~21 minutes).
    check("late metric spanning under 60s is not flagged sustained",
          !trend.value(QStringLiteral("sustainedGrowth")).toBool());
}

void testLiveProcessSnapshot()
{
    const ProcessMemorySnapshot live = ProcessMemorySnapshot::capture();
    const QJsonObject json = live.toJson();
    check("live process memory snapshot is valid", live.valid);
    check("live resident memory is non-zero", live.residentBytes > 0);
    check("live snapshot names its resident metric",
          !json.value(QStringLiteral("residentMetric")).toString().isEmpty());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testLinearGrowth();
    testFlatSeries();
    testBoundedRetention();
    testLateMetricDurationGate();
    testLiveProcessSnapshot();
    return g_failures == 0 ? 0 : 1;
}
