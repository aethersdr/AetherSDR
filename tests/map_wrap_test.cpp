#include <QGeoView/QGVGlobal.h>
#include <QGeoView/QGVCamera.h>
#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVMapQGView.h>
#include <QGeoView/QGVProjection.h>

#include <QApplication>
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
    ok &= expect(nearlyEqual(QGV::wrapProjectionX(180.0, -180.0, 360.0),
                             -180.0),
                 "+180 should wrap to -180");
    ok &= expect(nearlyEqual(QGV::wrapProjectionX(181.0, -180.0, 360.0),
                             -179.0),
                 "eastward camera motion should wrap across the dateline");
    ok &= expect(nearlyEqual(QGV::wrapProjectionX(-181.0, -180.0, 360.0),
                             179.0),
                 "westward camera motion should wrap across the dateline");
    ok &= expect(nearlyEqual(QGV::wrapProjectionX(901.0, -180.0, 360.0),
                             -179.0),
                 "multiple eastward world copies should normalize");

    ok &= expect(QGV::wrapTileX(2, -1) == 3,
                 "tile immediately west of the world should use the east edge");
    ok &= expect(QGV::wrapTileX(2, 4) == 0,
                 "tile immediately east of the world should use the west edge");
    ok &= expect(QGV::wrapTileX(3, 17) == 1,
                 "tile wrapping should handle multiple world copies");
    ok &= expect(QGV::wrapTileX(-1, 17) == 17,
                 "invalid zoom should leave tile x unchanged");

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

    return ok ? 0 : 1;
}
