#include "gui/WaterfallTimeMarkers.h"
#include <QDebug>
#include <limits>

using namespace AetherSDR;
int main()
{
    int failed = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { qCritical() << message; ++failed; }
    };
    for (const int seconds : kWaterfallMarkerIntervals) {
        if (seconds == 0) { continue; }
        const qint64 boundary = qint64(seconds) * 1000 * 100;
        QVector<WaterfallTimeRow> rows{{boundary, boundary - 1}};
        check(waterfallTimeMarkers(rows, 0, seconds, 0, 100).size() == 1, "exact clock crossing");
        rows[0] = {boundary - 1, boundary - 2};
        check(waterfallTimeMarkers(rows, 0, seconds, 0, 100).isEmpty(), "before boundary");
        rows[0] = {boundary + 2345, boundary - 1};
        const auto delayed = waterfallTimeMarkers(rows, 0, seconds, 0, 100);
        check(delayed.size() == 1 && delayed[0].timestampMs == boundary,
              "delayed row labels the exact clock boundary for every interval");
    }
    QVector<WaterfallTimeRow> ring(4);
    ring[3] = {60001, 60001}; // a batched tile must not repeat a timestamp
    ring[0] = {60001, 59999};
    ring[1] = {59999, 59000};
    ring[2] = {30001, 29999};
    const auto live = waterfallTimeMarkers(ring, 3, 15, 0.25, 40);
    check(live.size() == 2, "wrapped ring retains both crossings");
    if (live.size() == 2) {
        check(live[0].timestampMs == 60000 && live[0].y == 7.5, "fractional scroll follows row");
        check(live[1].y == 27.5, "oldest row retains predecessor");
    }
    const auto paused = waterfallTimeMarkers(ring, 3, 15, 0, 40);
    check(paused.size() == 2 && paused[0].y == 10, "paused viewport uses discrete rows");
    const auto resized = waterfallTimeMarkers(ring, 3, 15, 0.25, 80);
    check(resized.size() == 2 && resized[0].y == 15, "resize uses signal scaling");
    check(waterfallTimeMarkers(ring, 3, 60, 0, 40).size() == 1, "interval change reinterprets retained rows");
    check(waterfallTimeMarkers(ring, 3, 0, 0, 40).isEmpty(), "off");
    check(waterfallTimeMarkers(ring, 3, 17, 0, 40).isEmpty(), "invalid interval fails closed");
    check(waterfallTimeMarkers(ring, -1, 15, 0, 40).isEmpty(), "invalid head");
    check(waterfallTimeMarkers(ring, 3, 15, std::numeric_limits<double>::quiet_NaN(), 40).isEmpty(), "invalid scroll");
    check(waterfallTimeMarkers({{900000, 1000}}, 0, 15, 0, 40).size() == 1, "gap does not invent skipped rows");
    check(waterfallTimeMarkers({{1000, 900000}}, 0, 15, 0, 40).isEmpty(), "backward clock jump");
    check(waterfallTimeMarkers({{30000, 0}}, 0, 15, 0, 40).isEmpty(), "unstamped row");
    check(waterfallTimeMarkers({{60000, 60000}}, 0, 15, 0, 40).isEmpty(), "a row that did not advance the clock");
    check(waterfallTimeMarkers({{1800000, 1799999}}, 0, 1800, 0, 40).isEmpty(), "retired 30-minute interval is not accepted");
    check(waterfallTimeMarkers({{3600000, 3599999}}, 0, 3600, 0, 40).isEmpty(), "retired 1-hour interval is not accepted");
    check(waterfallTimeMarkers({{60000, 59999}}, 0, 15, 1, 40).isEmpty(), "not-yet-presented top row is clipped");
    return failed ? 1 : 0;
}
