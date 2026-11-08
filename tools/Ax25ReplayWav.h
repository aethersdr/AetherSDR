#pragma once

#include <QString>

#include <vector>

namespace AetherSDR::Ax25ReplayWav {

bool loadWav(const QString& path, std::vector<float>& samples, int& sampleRate, QString& err);

} // namespace AetherSDR::Ax25ReplayWav
