#pragma once

#include <QString>

namespace AetherSDR::SpotModeResolver {

QString extractSpotModeFromComment(const QString& comment);

QString inferSpotModeFromBand(double rxFreqMhz);

QString mapSpotModeToRadioMode(const QString& spotMode, double rxFreqMhz);

// All-in-one: explicit mode → comment parse → band-plan inference → radio mode.
// Returns empty string if no radio mode could be determined.
QString resolveSpotRadioMode(const QString& explicitMode,
                             const QString& comment,
                             double rxFreqMhz);

} // namespace AetherSDR::SpotModeResolver
