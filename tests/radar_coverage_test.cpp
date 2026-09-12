#include "gui/map/RadarCoverage.h"
#include <QCoreApplication>
#include <iostream>
using namespace AetherSDR;
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto check = [](bool ok, const char* label) { if (!ok) { std::cerr << label << '\n'; std::exit(1); } };
    for (const QPointF center : {QPointF(179.5, 40), QPointF(-122, 80), QPointF(20, -30)}) {
        const auto ring = radarRangeRing(center.y(), center.x(), 240);
        check(ring.size() == 97, "closed ring count");
        for (const QPointF p : ring) {
            constexpr double d = 3.141592653589793 / 180;
            const double a = std::pow(std::sin((p.y() - center.y()) * d / 2), 2)
                + std::cos(center.y()*d)*std::cos(p.y()*d)*std::pow(std::sin((p.x()-center.x())*d/2),2);
            check(std::abs(6371.0088 * 2 * std::asin(std::sqrt(a)) - 240) < 0.0001, "geographic radius");
            check(p.x() >= -180 && p.x() <= 180, "wrapped longitude");
        }
    }
    check(radarRangeRing(91, 0, 20).isEmpty(), "invalid latitude");
    const QByteArray sites = R"([{"odimcode":"a","status":"1","location":"Site","country":"Test","latitude":"51","longitude":"5","maxrange":"240"},{"odimcode":"bad","status":"1","latitude":"oops","longitude":"5"}])";
    const auto parsed = parseRadarSites(sites, true);
    check(parsed.size() == 1 && parsed[0].rangeKm == 240, "OPERA validation");
    check(parseRadarSites(QByteArray(3*1024*1024, 'x'), true).isEmpty(), "allocation cap");
    const QByteArray unknown = R"({"features":[{"geometry":{"type":"Point","coordinates":[-79,35]},"properties":{"id":"KTEST","name":"Test","stationType":"other"}}]})";
    check(parseRadarSites(unknown, false).first().ring.isEmpty(), "unknown range not invented");
    return 0;
}
