// SpectrumTraceBins::snapshot, behind `get tracedebug <pan> bins` (#5806):
// the bins come back one for one, with the geometry to place them.

#include "gui/SpectrumTraceBins.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>

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

    // FrameCapture: the frame as received, refused until the geometry settles.
    {
        using Capture = SpectrumTraceBins::FrameCapture;
        constexpr quint64 k = Capture::kSettledFrames;
        const auto frame = [](float offset) {
            QVector<float> f(64);
            for (int i = 0; i < f.size(); ++i)
                f[i] = -120.0f + offset + 0.25f * static_cast<float>(i);
            return f;
        };
        const auto binsOf = [](const QVariantMap& m) {
            QVector<float> out;
            for (const QVariant& v : m.value(QStringLiteral("bins")).toList())
                out.append(static_cast<float>(v.toDouble()));
            return out;
        };
        Capture c;
        check(c.refusal(14.1, 0.2, false, false).contains(QLatin1String("no frame")),
              "capture: no frame yet is refused");
        for (quint64 i = 1; i <= k; ++i)
            c.ingest(frame(static_cast<float>(i)), i, 14.1, 0.2, false);
        check(c.refusal(14.1, 0.2, false, false).isEmpty(), "capture: kSettledFrames frames settle");
        check(binsOf(c.snapshot()) == frame(static_cast<float>(k)), "capture: the last frame comes back as fed");
        check(c.snapshot().value(QStringLiteral("fftFrameSequence")).toULongLong() == k,
              "capture: with that frame's sequence");
        check(c.snapshot().value(QStringLiteral("ageMs")).toLongLong() >= 0, "capture: ageMs is reported");

        check(!c.refusal(14.15, 0.2, false, false).isEmpty(), "capture: a centre other than the frame's is refused");
        c.noteGeometryChange();
        check(c.refusal(14.1, 0.2, false, false).contains(QLatin1String("0 frames ago")),
              "capture: refused right after a reported change");
        for (quint64 i = 1; i < k; ++i) {
            c.ingest(frame(10.0f + static_cast<float>(i)), k + i, 14.1, 0.2, false);
            check(!c.refusal(14.1, 0.2, false, false).isEmpty(), "capture: refused below kSettledFrames");
        }
        c.ingest(frame(20.0f), 2 * k, 14.1, 0.2, false);
        check(c.refusal(14.1, 0.2, false, false).isEmpty() && binsOf(c.snapshot()) == frame(20.0f),
              "capture: the kSettledFrames-th frame is returned");

        c.ingest(frame(30.0f), 2 * k + 1, 14.2, 0.1, false);  // a change nobody reported
        check(!c.refusal(14.2, 0.1, false, false).isEmpty(),
              "capture: a geometry change seen only at ingest restarts the count");
        check(c.refusal(14.2, 0.1, true, false).contains(QLatin1String("animation")),
              "capture: a running pan animation is refused");
        check(c.refusal(14.2, 0.1, false, true).contains(QLatin1String("in progress")),
              "capture: a gesture in progress is refused");
        for (quint64 i = 0; i < k; ++i)
            c.ingest(frame(40.0f), 3 * k + i, 14.2, 0.1, true);
        check(c.refusal(14.2, 0.1, false, false).contains(QLatin1String("dBm range")),
              "capture: a frame rebased across a dBm range change is refused");
        c.ingest({}, 9999, 14.2, 0.1, false);
        check(c.snapshot().value(QStringLiteral("fftFrameSequence")).toULongLong() != 9999,
              "capture: an empty frame is not captured");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "spectrum_trace_bins_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
