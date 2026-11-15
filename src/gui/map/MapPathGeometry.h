#pragma once

#include <QtMath>

namespace AetherSDR::MapPathGeometry {

// Return the horizontally repeated-world copy of canonicalX nearest to the
// preceding path sample. This turns an antimeridian jump into a continuous
// projected curve while leaving ordinary samples unchanged.
inline double unwrapX(double canonicalX, double previousX, double worldWidth)
{
    if (worldWidth <= 0.0) {
        return canonicalX;
    }
    return canonicalX
         + qRound((previousX - canonicalX) / worldWidth) * worldWidth;
}

} // namespace AetherSDR::MapPathGeometry
