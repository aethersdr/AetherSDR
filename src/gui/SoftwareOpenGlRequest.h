#pragma once

#include <QString>

namespace AetherSDR {

// AETHER_NO_GPU is the documented runtime escape hatch. QT_OPENGL=software
// may also be inherited from the launcher, so every QRhiWidget in a top-level
// window must make the same backend choice for either request.
inline bool softwareOpenGlRequested()
{
    return qEnvironmentVariableIsSet("AETHER_NO_GPU")
        || qEnvironmentVariable("QT_OPENGL").compare(
               QStringLiteral("software"), Qt::CaseInsensitive) == 0;
}

} // namespace AetherSDR
