// SpectrumTraceBins::snapshot, behind `get tracedebug <pan> bins` (#5806):
// the bins come back one for one, with the geometry to place them.

#include "gui/SpectrumTraceBins.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <cstdio>
#include <limits>

using namespace AetherSDR;

static int g_failures = 0;

static void check(bool ok, const char* what)
{
    std::fprintf(stderr, "%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

int main()
{
    QVector<float> bins(1024);
    for (int i = 0; i < bins.size(); ++i)
        bins[i] = -120.0f + 0.05f * static_cast<float>(i);
    bins[100] = std::numeric_limits<float>::quiet_NaN();
    bins[200] = -std::numeric_limits<float>::infinity();

    const QVariantMap m = SpectrumTraceBins::snapshot(bins, 3.775, 0.384, 42);
    const QJsonObject o = QJsonObject::fromVariantMap(m);
    const QJsonArray a = o.value(QStringLiteral("bins")).toArray();

    check(o.value(QStringLiteral("count")).toInt() == 1024, "count is the number of bins");
    check(a.size() == 1024, "every bin is returned, not a statistic");
    check(o.value(QStringLiteral("finiteCount")).toInt() == 1022, "finiteCount leaves out the two non-finite bins");
    check(std::fabs(a.at(0).toDouble() + 120.0) < 1e-6, "bin 0 keeps its value");
    check(std::fabs(a.at(1023).toDouble() - (-120.0 + 0.05 * 1023)) < 1e-4, "the last bin keeps its value");
    check(a.at(100).isNull() && a.at(200).isNull(), "a non-finite bin is null, so the JSON stays valid");
    check(std::fabs(o.value(QStringLiteral("centerHz")).toDouble() - 3'775'000.0) < 1e-3, "centerHz");
    check(std::fabs(o.value(QStringLiteral("spanHz")).toDouble() - 384'000.0) < 1e-3, "spanHz");
    check(std::fabs(o.value(QStringLiteral("lowHz")).toDouble() - 3'583'000.0) < 1e-3, "lowHz is the span's low edge");
    check(std::fabs(o.value(QStringLiteral("binHz")).toDouble() - 375.0) < 1e-9, "binHz is span / count");
    check(o.value(QStringLiteral("fftFrameSequence")).toInteger() == 42, "fftFrameSequence names the frame");
    check(!QJsonDocument(o).toJson(QJsonDocument::Compact).isEmpty(), "the reply serialises");

    const QJsonObject empty = QJsonObject::fromVariantMap(
        SpectrumTraceBins::snapshot({}, 14.1, 0.192, 0));
    check(empty.value(QStringLiteral("count")).toInt() == 0
              && empty.value(QStringLiteral("binHz")).toDouble() == 0.0
              && empty.value(QStringLiteral("bins")).toArray().isEmpty(),
          "no frame yet: zero bins and binHz 0, not a division by zero");

    if (g_failures == 0)
        std::fprintf(stderr, "spectrum_trace_bins_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
