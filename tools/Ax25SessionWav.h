#pragma once

#include <QString>

#include <vector>

namespace AetherSDR::Ax25SessionWav {

// Loads mono float32 captures for the offline session analyzer.
bool loadWav(const QString& path, std::vector<float>& samples, int& sampleRate, QString& err);

} // namespace AetherSDR::Ax25SessionWav
