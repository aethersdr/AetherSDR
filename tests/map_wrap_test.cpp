#include <QGeoView/QGVGlobal.h>
#include <QGeoView/QGVCamera.h>
#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVMapQGView.h>
#include <QGeoView/QGVProjection.h>

#include <QApplication>
#include <QPoint>
#include <QWheelEvent>
#include <QtGlobal>

#include <cmath>
#include <iostream>

namespace {

bool nearlyEqual(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-9;
}

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    bool ok = true;

    ok &= expect(QGV::wrapTileX(2, -1) == 3,
                 "tile immediately west of the world should use the east edge");
    ok &= expect(QGV::wrapTileX(2, 4) == 0,
                 "tile immediately east of the world should use the west edge");
    ok &= expect(QGV::wrapTileX(3, 17) == 1,
                 "tile wrapping should handle multiple world copies");
    ok &= expect(QGV::wrapTileX(-1, 17) == 17,
                 "invalid zoom should leave tile x unchanged");

    // GeoTilePos::parent() must FLOOR, not truncate. Wrap is what first makes
    // tile x negative, and contains() is built on parent(), so a truncating
    // divide reports a western-copy tile as a child of a base-world tile —
    // which makes removeWhenCovered() evict a fallback tile while the base
    // world behind it is still half empty, leaving a hole at the antimeridian.
    ok &= expect(QGV::GeoTilePos(5, QPoint(-1, 0)).parent(4).pos().x() == -1,
                 "parent() of tile x=-1 should floor to -1, not truncate to 0");
    ok &= expect(QGV::GeoTilePos(5, QPoint(-3, 0)).parent(4).pos().x() == -2,
                 "parent() of tile x=-3 should floor to -2, not truncate to -1");
    ok &= expect(QGV::GeoTilePos(4, QPoint(-1, 0))
                         .contains(QGV::GeoTilePos(5, QPoint(-1, 0))),
                 "a western-copy tile should be a child of its own parent");
    ok &= expect(!QGV::GeoTilePos(4, QPoint(0, 0))
                          .contains(QGV::GeoTilePos(5, QPoint(-1, 0))),
                 "a western-copy tile must not count as a base-world child");

    // Exercise QGraphicsView's real centering constraint at the whole-world
    // scale. A wide viewport is wider than two worlds here; without the
    // moving wrap scene rectangle, centerOn() clamps before the dateline and
    // the tile surface goes blank even though the coordinate math is right.
    QGVMap map;
    map.resize(1262, 572);
    map.show();
    app.processEvents();
    map.geoView()->setHorizontalWrapEnabled(true);
    const QRectF world = map.getProjection()->boundaryProjRect();
    const double wholeWorldScale = 572.0 / world.height();
    map.geoView()->setScaleLimits(wholeWorldScale, 16.0);
    const QPointF beyondWest(world.left() - world.width() * 0.01,
                             world.center().y());
    map.cameraTo(QGVCameraActions(&map)
                     .scaleTo(wholeWorldScale)
                     .moveTo(beyondWest),
                 false);
    const double expectedWest = beyondWest.x();
    const double actualWest = map.getCamera().projCenter().x();
    const double centerErrorPixels =
        std::abs(actualWest - expectedWest) * wholeWorldScale;
    if (centerErrorPixels > 1.0) {
        std::cerr << "wrapped camera center: actual=" << actualWest
                  << " expected=" << expectedWest
                  << " errorPixels=" << centerErrorPixels << '\n';
    }
    ok &= expect(centerErrorPixels <= 1.0,
                 "wide wrapped camera should reach the dateline without scene clamping");

    // Zoom symmetry: n notches in followed by n notches out must land back on
    // the scale we started from. Upstream's asymmetric in/out exponents lost
    // ~11% per round trip, so the operator drifted away from their own view
    // just by rocking the wheel. This is the invariant the single symmetric
    // curve establishes, so pin it rather than the constants.
    map.geoView()->setScaleLimits(1e-6, 1e3);
    map.cameraTo(QGVCameraActions(&map).scaleTo(1.0), false);
    app.processEvents();
    const double startScale = map.getCamera().scale();
    const auto wheel = [&](int notches) {
        for (int i = 0; i < std::abs(notches); ++i) {
            const QPointF pos(map.geoView()->width() / 2.0,
                              map.geoView()->height() / 2.0);
            QWheelEvent event(pos, map.geoView()->mapToGlobal(pos.toPoint()),
                              QPoint(0, 0),
                              QPoint(0, notches > 0 ? 120 : -120),
                              Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QApplication::sendEvent(map.geoView()->viewport(), &event);
            app.processEvents();
        }
    };
    wheel(4);
    const double zoomedIn = map.getCamera().scale();
    wheel(-4);
    const double roundTrip = map.getCamera().scale();
    ok &= expect(zoomedIn > startScale * 1.5,
                 "four wheel notches in should zoom in appreciably");
    if (!nearlyEqual(roundTrip / startScale, 1.0)) {
        std::cerr << "zoom round trip: start=" << startScale
                  << " in=" << zoomedIn << " back=" << roundTrip << '\n';
    }
    ok &= expect(nearlyEqual(roundTrip / startScale, 1.0),
                 "equal wheel notches in and out should return the same scale");

    return ok ? 0 : 1;
}
