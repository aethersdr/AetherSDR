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
