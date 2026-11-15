#pragma once

namespace AetherSDR {

// Apply the same opacity once in live tiles, flat playback, and globe rendering.
// Keep the painter value in double precision; convert only at the GL uniform.
inline constexpr double kWeatherRadarOpacity = 0.78;

} // namespace AetherSDR
