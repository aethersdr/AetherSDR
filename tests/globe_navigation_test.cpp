#include "gui/map/GlobeNavigation.h"

#include <cmath>
#include <iostream>

using namespace AetherSDR;

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

bool nearlyEqual(double actual, double expected, double tolerance = 0.001)
{
    return std::abs(actual - expected) <= tolerance;
}

} // namespace

int main()
{
    bool ok = true;
    GlobeNavigation navigation;

    const float defaultDragScale =
        GlobeNavigation::screenTrackedDegreesPerPixel(3.8F, 600, 34.0F);
    const float closeDragScale =
        GlobeNavigation::screenTrackedDegreesPerPixel(1.4F, 600, 34.0F);
    ok &= check(nearlyEqual(defaultDragScale, 0.16347, 0.0001),
                "drag scale follows the perspective projection");
    ok &= check(closeDragScale < defaultDragScale * 0.15F,
                "zoomed-in drag slows with the enlarged globe surface");
    ok &= check(nearlyEqual(
                    GlobeNavigation::screenTrackedDegreesPerPixel(
                        3.8F, 0, 34.0F),
                    0.0),
                "an unavailable viewport produces no drag motion");

    navigation.reset(35.0, 190.0);
    // Project the initially grabbed point after a real screen drag. Testing
    // only degrees-per-pixel missed latitude, explicit roll and off-centre
    // perspective errors in the production mouse path.
    for (const double latitude : {0.0, 45.0, 60.0}) {
        for (const float roll : {0.0F, 35.0F, 90.0F}) {
            for (const double distance : {1.08, 1.4, 3.8, 6.0}) {
                navigation.reset(latitude, -90.0);
                navigation.applyRollDelta(roll);
                const QVector3D grabbed = navigation.rotation().conjugated().rotatedVector(
                    QVector3D(0.0F, 0.0F, 1.0F));
                ok &= check(navigation.applyScreenDrag(QPointF(500, 300), QPointF(520, 310),
                                QSizeF(1000, 600), distance, 34.0),
                            "screen drag must find a valid globe orientation");
                const QVector3D moved = navigation.rotation().rotatedVector(grabbed);
                const double focal = 600.0 / (2.0 * std::tan(17.0 * std::numbers::pi / 180.0));
                ok &= check(nearlyEqual(500 + focal * moved.x() / (distance - moved.z()), 520, 0.02)
                                && nearlyEqual(300 - focal * moved.y() / (distance - moved.z()), 310, 0.02),
                            "grabbed surface point must follow pointer across latitude roll and zoom");
                ok &= check(nearlyEqual(navigation.rollDegrees(), roll),
                            "surface dragging preserves explicit tilt");
                // Use the moved, off-centre point to reverse the drag.
                navigation.applyScreenDrag(QPointF(520, 310), QPointF(500, 300),
                                           QSizeF(1000, 600), distance, 34.0);
                ok &= check(nearlyEqual(navigation.latitude(), latitude, 0.002)
                                && nearlyEqual(navigation.longitude(), -90.0, 0.002),
                            "off-centre reverse drag returns to the original centre");
            }
        }
    }
    navigation.reset(35.0, 190.0);
    ok &= check(nearlyEqual(navigation.latitude(), 35.0),
                "reset preserves latitude");
    ok &= check(nearlyEqual(navigation.longitude(), -170.0),
                "reset normalizes longitude");
    ok &= check(nearlyEqual(navigation.rollDegrees(), 0.0),
                "reset levels the globe");

    navigation.applyRollDelta(25.0F);
    navigation.applyDragDelta(QPointF(100.0, -20.0), 0.28F);
    ok &= check(nearlyEqual(navigation.rollDegrees(), 25.0),
                "ordinary drag does not change explicit roll");
    ok &= check(nearlyEqual(navigation.latitude(), 29.4),
                "drag updates latitude");
    ok &= check(nearlyEqual(navigation.longitude(), 162.0),
                "drag updates longitude");

    navigation.applyDragDelta(QPointF(0.0, 10000.0), 0.28F);
    ok &= check(nearlyEqual(navigation.latitude(), 89.5),
                "drag clamps latitude before the pole");
    ok &= check(nearlyEqual(navigation.rotation().length(), 1.0),
                "navigation maintains a normalized rotation");

    navigation.reset(-12.0, 45.0);
    ok &= check(nearlyEqual(navigation.rollDegrees(), 0.0),
                "home reset clears explicit roll");

    return ok ? 0 : 1;
}
