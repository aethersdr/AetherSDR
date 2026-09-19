#pragma once

#include <QString>

namespace AetherSDR::DemoRadio {

// Backend-only compatibility data for the in-process Demo radio and its
// synthetic Flex connection. Neither consumer needs the other's implementation.
inline QString serial()
{
    return QStringLiteral("DEMO-0001");
}

// Flex's line_duration field is a 1..100 rate, NOT a millisecond interval.
// Demo produces its own cadence; 100 leaves those rows ungated (#4606).
inline constexpr int kWaterfallRate = 100;

} // namespace AetherSDR::DemoRadio
